// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// ACTIONS: an application names what it wants ("teleport", "grab"), suggests where each comes from on
// the controllers it knows ("/user/hand/right/input/trigger/value" on Touch), and reads the actions back
// after each xrSyncActions.
//
// ONE SIMULATED CONTROLLER SERVES EVERY PROFILE (DESIGN.md, "What the simulated headset is"). The
// controllers claim to be the first suggested of a preference list -- the profiles applications are most
// often written for -- and every binding is answered by what its path names: a trigger is the trigger
// whichever controller has it, a thumbstick or a trackpad the stick, `a` and `x` the primary button. A
// binding naming something the simulator does not have (a thumbrest, a finger curl) reads as untouched.
//
// XR_EXT_HAND_INTERACTION (DESIGN.md, "Hand interaction"): its profile for hands is answered by the same
// controller, through the mapping the extension asks of a runtime with controllers -- pinch and
// aim-activate are the trigger, grasp the grip, and every ready_ext is true, because a trigger is always
// ready. Its pinch and poke poses are added to every controller profile too, from the hand hand.c draws.
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const PREFERRED_PROFILES[] = {
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/meta/touch_controller_plus",
    "/interaction_profiles/valve/index_controller",
    "/interaction_profiles/htc/vive_controller",
    "/interaction_profiles/microsoft/motion_controller",
    "/interaction_profiles/khr/simple_controller",
    // Last: these are controllers, so an application that also suggested one of theirs gets that.
    "/interaction_profiles/ext/hand_interaction_ext",
    NULL,
};

static bool append(void *array_ptr, size_t *n, const void *item, size_t size)
{
    void **array = array_ptr;
    void *g = realloc(*array, (*n + 1) * size);
    if (g == NULL) return false;
    memcpy((char *)g + *n * size, item, size);
    *array = g;
    (*n)++;
    return true;
}

static void remove_ptr(void *array_ptr, size_t *n, const void *item)
{
    void ***array = array_ptr;
    for (size_t i = 0; i < *n; i++)
        if ((*array)[i] == item) {
            memmove(*array + i, *array + i + 1, (*n - i - 1) * sizeof **array);
            (*n)--;
            return;
        }
}

static bool any_attached(struct instance *in)
{
    for (size_t i = 0; i < in->nsets; i++)
        if (in->sets[i]->attached) return true;
    return false;
}

// ---- action sets and actions ---------------------------------------------------------------------

static void set_fini(struct object *o)
{
    struct action_set *set = (struct action_set *)o;
    struct instance *in = (struct instance *)o->instance;
    remove_ptr(&in->sets, &in->nsets, set);
    if (in->session != NULL) remove_ptr(&in->session->attached, &in->session->nattached, set);
    free(set->actions);   // the actions themselves are its children, destroyed before it
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateActionSet(XrInstance instance, const XrActionSetCreateInfo *info, XrActionSet *out)
{
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_ACTION_SET_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (info->actionSetName[0] == '\0') return XR_ERROR_NAME_INVALID;
    if (info->localizedActionSetName[0] == '\0') return XR_ERROR_LOCALIZED_NAME_INVALID;
    for (size_t i = 0; i < in->nsets; i++)
        if (strcmp(in->sets[i]->name, info->actionSetName) == 0) return XR_ERROR_NAME_DUPLICATED;
    struct action_set *set = make(sizeof *set, MAGIC_ACTION_SET, &in->o, NULL);
    if (set == NULL) return XR_ERROR_OUT_OF_MEMORY;
    snprintf(set->name, sizeof set->name, "%s", info->actionSetName);
    set->priority = info->priority;
    if (!append(&in->sets, &in->nsets, &set, sizeof set)) { destroy(&set->o); return XR_ERROR_OUT_OF_MEMORY; }
    set->o.fini = set_fini;
    *out = (XrActionSet)(uintptr_t)set;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroyActionSet(XrActionSet handle)
{
    struct object *o = find(H(handle), MAGIC_ACTION_SET);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

static void action_fini(struct object *o)
{
    struct action *a = (struct action *)o;
    remove_ptr(&a->set->actions, &a->set->nactions, a);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateAction(XrActionSet handle, const XrActionCreateInfo *info, XrAction *out)
{
    struct action_set *set = (struct action_set *)find(H(handle), MAGIC_ACTION_SET);
    if (set == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_ACTION_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (info->actionName[0] == '\0') return XR_ERROR_NAME_INVALID;
    if (info->localizedActionName[0] == '\0') return XR_ERROR_LOCALIZED_NAME_INVALID;
    if (set->attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    switch (info->actionType) {
        case XR_ACTION_TYPE_BOOLEAN_INPUT: case XR_ACTION_TYPE_FLOAT_INPUT: case XR_ACTION_TYPE_VECTOR2F_INPUT:
        case XR_ACTION_TYPE_POSE_INPUT: case XR_ACTION_TYPE_VIBRATION_OUTPUT: break;
        default: return XR_ERROR_VALIDATION_FAILURE;
    }
    if (info->countSubactionPaths > 8 || (info->countSubactionPaths > 0 && info->subactionPaths == NULL))
        return XR_ERROR_VALIDATION_FAILURE;
    for (size_t i = 0; i < set->nactions; i++)
        if (strcmp(set->actions[i]->name, info->actionName) == 0) return XR_ERROR_NAME_DUPLICATED;
    // SUBACTION PATHS are the top-level users: the hands, the head, a gamepad.
    for (uint32_t i = 0; i < info->countSubactionPaths; i++) {
        const char *p = path_string(info->subactionPaths[i]);
        if (p == NULL) return XR_ERROR_PATH_INVALID;
        if (strcmp(p, "/user/hand/left") != 0 && strcmp(p, "/user/hand/right") != 0 && strcmp(p, "/user/head") != 0 &&
            strcmp(p, "/user/gamepad") != 0 && strcmp(p, "/user/treadmill") != 0)
            return XR_ERROR_PATH_UNSUPPORTED;
    }
    struct action *a = make(sizeof *a, MAGIC_ACTION, set->o.instance, &set->o);
    if (a == NULL) return XR_ERROR_OUT_OF_MEMORY;
    a->set = set;
    snprintf(a->name, sizeof a->name, "%s", info->actionName);
    a->type = info->actionType;
    a->nsub = info->countSubactionPaths;
    for (uint32_t i = 0; i < a->nsub; i++) a->sub[i] = info->subactionPaths[i];
    if (!append(&set->actions, &set->nactions, &a, sizeof a)) { destroy(&a->o); return XR_ERROR_OUT_OF_MEMORY; }
    a->o.fini = action_fini;
    *out = (XrAction)(uintptr_t)a;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroyAction(XrAction handle)
{
    struct object *o = find(H(handle), MAGIC_ACTION);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

// ---- suggestions and the chosen profile ----------------------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SuggestInteractionProfileBindings(XrInstance instance,
                                                                           const XrInteractionProfileSuggestedBinding *b)
{
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (b == NULL || b->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) return XR_ERROR_VALIDATION_FAILURE;
    if (b->countSuggestedBindings == 0 || b->suggestedBindings == NULL) return XR_ERROR_VALIDATION_FAILURE;
    const char *profile = path_string(b->interactionProfile);
    if (profile == NULL) return XR_ERROR_PATH_INVALID;
    if (strncmp(profile, "/interaction_profiles/", 22) != 0) return XR_ERROR_PATH_UNSUPPORTED;
    // An extension's profile and paths exist only while it is enabled.
    if (!in->hand_interaction && strcmp(profile, "/interaction_profiles/ext/hand_interaction_ext") == 0)
        return XR_ERROR_PATH_UNSUPPORTED;
    if (any_attached(in)) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < b->countSuggestedBindings; i++) {
        if (find(H(b->suggestedBindings[i].action), MAGIC_ACTION) == NULL) return XR_ERROR_HANDLE_INVALID;
        const char *p = path_string(b->suggestedBindings[i].binding);
        if (p == NULL) return XR_ERROR_PATH_INVALID;
        if (strncmp(p, "/user/", 6) != 0) return XR_ERROR_PATH_UNSUPPORTED;
        if (!in->hand_interaction && (strstr(p, "/input/pinch_ext/") != NULL || strstr(p, "/input/poke_ext/") != NULL ||
                                      strstr(p, "/input/aim_activate_ext/") != NULL || strstr(p, "/input/grasp_ext/") != NULL))
            return XR_ERROR_PATH_UNSUPPORTED;
    }
    XrActionSuggestedBinding *copy = malloc(b->countSuggestedBindings * sizeof *copy);
    if (copy == NULL) return XR_ERROR_OUT_OF_MEMORY;
    memcpy(copy, b->suggestedBindings, b->countSuggestedBindings * sizeof *copy);
    for (size_t i = 0; i < in->nsuggested; i++)
        if (in->suggested[i].profile == b->interactionProfile) {
            free(in->suggested[i].b);
            in->suggested[i].b = copy;
            in->suggested[i].n = b->countSuggestedBindings;
            return XR_SUCCESS;
        }
    const struct suggestion s = { b->interactionProfile, copy, b->countSuggestedBindings };
    if (!append(&in->suggested, &in->nsuggested, &s, sizeof s)) { free(copy); return XR_ERROR_OUT_OF_MEMORY; }
    say("bindings suggested for %s (%u)", profile, b->countSuggestedBindings);
    return XR_SUCCESS;
}

void input_choose_profile(struct session *s)
{
    struct instance *in = s->in;
    const struct suggestion *chosen = NULL;
    for (int p = 0; PREFERRED_PROFILES[p] != NULL && chosen == NULL; p++)
        for (size_t i = 0; i < in->nsuggested; i++) {
            const char *name = path_string(in->suggested[i].profile);
            if (name != NULL && strcmp(name, PREFERRED_PROFILES[p]) == 0) { chosen = &in->suggested[i]; break; }
        }
    if (chosen == NULL && in->nsuggested > 0) chosen = &in->suggested[0];
    s->profile = chosen != NULL ? chosen->profile : XR_NULL_PATH;
    for (size_t i = 0; i < s->nattached; i++) {
        struct action_set *set = s->attached[i];
        for (size_t k = 0; k < set->nactions; k++) {
            struct action *a = set->actions[k];
            a->nbound = 0;
            for (uint32_t j = 0; chosen != NULL && j < chosen->n; j++)
                if ((struct action *)(uintptr_t)chosen->b[j].action == a && a->nbound < 16) a->bound[a->nbound++] = chosen->b[j].binding;
        }
    }
    say("the controllers are %s", chosen != NULL ? path_string(chosen->profile) : "nothing (no bindings were suggested)");
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo *info)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO || info->countActionSets == 0 || info->actionSets == NULL)
        return XR_ERROR_VALIDATION_FAILURE;
    if (s->attached != NULL) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < info->countActionSets; i++)
        if (find(H(info->actionSets[i]), MAGIC_ACTION_SET) == NULL) return XR_ERROR_HANDLE_INVALID;
    s->attached = calloc(info->countActionSets, sizeof *s->attached);
    if (s->attached == NULL) return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < info->countActionSets; i++) {
        struct action_set *set = (struct action_set *)(uintptr_t)info->actionSets[i];
        set->attached = true;
        s->attached[s->nattached++] = set;
    }
    input_choose_profile(s);
    const XrEventDataInteractionProfileChanged e = {
        .type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED,
        .session = session,
    };
    post_event(s->in, &e, sizeof e);
    session_status(s);
    return XR_SUCCESS;
}

static bool attached_to(struct session *s, struct action_set *set)
{
    for (size_t i = 0; i < s->nattached; i++)
        if (s->attached[i] == set) return true;
    return false;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetCurrentInteractionProfile(XrSession session, XrPath top, XrInteractionProfileState *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (out == NULL || out->type != XR_TYPE_INTERACTION_PROFILE_STATE) return XR_ERROR_VALIDATION_FAILURE;
    if (s->attached == NULL) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    const char *p = path_string(top);
    if (p == NULL) return XR_ERROR_PATH_INVALID;
    const bool hand = strcmp(p, "/user/hand/left") == 0 || strcmp(p, "/user/hand/right") == 0;
    if (!hand && strcmp(p, "/user/head") != 0 && strcmp(p, "/user/gamepad") != 0) return XR_ERROR_PATH_UNSUPPORTED;
    out->interactionProfile = hand ? s->profile : XR_NULL_PATH;
    return XR_SUCCESS;
}

// ---- what a binding reads ------------------------------------------------------------------------

// A binding path taken apart: which hand (-1 for none), and what it names -- "trigger" and "value" from
// /user/hand/left/input/trigger/value. `output` for a haptic.
struct source { int hand; char id[48]; char comp[24]; bool output; };

static bool parse_source(XrPath path, struct source *out)
{
    const char *p = path_string(path);
    if (p == NULL) return false;
    memset(out, 0, sizeof *out);
    if (strncmp(p, "/user/hand/left/", 16) == 0) { out->hand = 0; p += 16; }
    else if (strncmp(p, "/user/hand/right/", 17) == 0) { out->hand = 1; p += 17; }
    else return false;
    if (strncmp(p, "input/", 6) == 0) p += 6;
    else if (strncmp(p, "output/", 7) == 0) { p += 7; out->output = true; }
    else return false;
    const char *slash = strchr(p, '/');
    const size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
    snprintf(out->id, sizeof out->id, "%.*s", (int)n, p);
    if (slash != NULL) snprintf(out->comp, sizeof out->comp, "%s", slash + 1);
    return true;
}

static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static uint32_t button_of(const char *id)
{
    if (is(id, "a") || is(id, "x")) return OXS_BUTTON_PRIMARY;
    if (is(id, "b") || is(id, "y")) return OXS_BUTTON_SECONDARY;
    if (is(id, "menu") || is(id, "back") || is(id, "start")) return OXS_BUTTON_MENU;
    if (is(id, "system") || is(id, "home")) return OXS_BUTTON_SYSTEM;
    return 0;
}

static bool is_stick(const char *id) { return is(id, "thumbstick") || is(id, "joystick") || is(id, "trackpad"); }

// A source as a number: 0..1 for a button, a trigger or a grip; -1..1 for one axis of a stick.
static float scalar(const struct oxs_hand *h, const struct source *src)
{
    const char *id = src->id, *c = src->comp;
    if (is(id, "pinch_ext") || is(id, "aim_activate_ext") || is(id, "grasp_ext")) {
        if (is(c, "ready_ext")) return 1.0f;
        return is(id, "grasp_ext") ? h->squeeze : h->trigger;
    }
    if (is(id, "trigger") || is(id, "select")) {
        if (is(c, "click") || is(id, "select")) return h->trigger >= 0.5f ? 1.0f : 0.0f;
        if (is(c, "touch") || is(c, "proximity")) return h->trigger > 0.05f ? 1.0f : 0.0f;
        return h->trigger;
    }
    if (is(id, "squeeze") || is(id, "grip")) {
        if (is(c, "click")) return h->squeeze >= 0.5f ? 1.0f : 0.0f;
        if (is(c, "touch")) return h->squeeze > 0.05f ? 1.0f : 0.0f;
        return h->squeeze;
    }
    if (is_stick(id)) {
        if (is(c, "x")) return h->stick[0];
        if (is(c, "y")) return h->stick[1];
        if (is(c, "click") || is(c, "force")) return (h->buttons & OXS_BUTTON_STICK) ? 1.0f : 0.0f;
        if (is(c, "touch")) return (h->buttons & OXS_BUTTON_STICK) || h->stick[0] != 0 || h->stick[1] != 0 ? 1.0f : 0.0f;
        return 0;
    }
    const uint32_t b = button_of(id);
    return b != 0 && (h->buttons & b) ? 1.0f : 0.0f;
}

// The state of one action, from the bindings that match `sub` (every binding when it is XR_NULL_PATH).
static struct action_state evaluate(const struct action *a, XrPath sub, const struct oxs_tracking *t)
{
    struct action_state st = { 0 };
    const char *subs = sub != XR_NULL_PATH ? path_string(sub) : NULL;
    for (uint32_t i = 0; i < a->nbound; i++) {
        const char *bp = path_string(a->bound[i]);
        if (bp == NULL || (subs != NULL && strncmp(bp, subs, strlen(subs)) != 0)) continue;
        struct source src;
        if (!parse_source(a->bound[i], &src)) continue;
        st.active = true;
        const struct oxs_hand *h = &t->hand[src.hand];
        switch (a->type) {
            case XR_ACTION_TYPE_BOOLEAN_INPUT: st.b = st.b || scalar(h, &src) >= 0.5f; break;
            case XR_ACTION_TYPE_FLOAT_INPUT: {
                const float v = scalar(h, &src);
                if ((v < 0 ? -v : v) > (st.f < 0 ? -st.f : st.f)) st.f = v;
                break;
            }
            case XR_ACTION_TYPE_VECTOR2F_INPUT:
                if (is_stick(src.id) && src.comp[0] == '\0') {
                    const float len = h->stick[0] * h->stick[0] + h->stick[1] * h->stick[1];
                    if (len > st.v.x * st.v.x + st.v.y * st.v.y) st.v = (XrVector2f){ h->stick[0], h->stick[1] };
                }
                break;
            default: break;
        }
    }
    return st;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_SyncActions(XrSession session, const XrActionsSyncInfo *info)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_ACTIONS_SYNC_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (info->countActiveActionSets > 0 && info->activeActionSets == NULL) return XR_ERROR_VALIDATION_FAILURE;
    for (uint32_t i = 0; i < info->countActiveActionSets; i++) {
        struct action_set *set = (struct action_set *)find(H(info->activeActionSets[i].actionSet), MAGIC_ACTION_SET);
        if (set == NULL) return XR_ERROR_HANDLE_INVALID;
        if (!attached_to(s, set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    const bool focused = s->state == XR_SESSION_STATE_FOCUSED;
    const struct oxs_tracking t = session_tracking(s);
    const XrTime now = now_ns();
    for (size_t i = 0; i < s->nattached; i++) {
        struct action_set *set = s->attached[i];
        XrPath only[8];   // the subaction paths this set is active for, unless it is active for all
        uint32_t nonly = 0;
        bool active = false, all = false;
        for (uint32_t k = 0; k < info->countActiveActionSets; k++)
            if ((struct action_set *)(uintptr_t)info->activeActionSets[k].actionSet == set) {
                active = true;
                if (info->activeActionSets[k].subactionPath == XR_NULL_PATH) all = true;
                else if (nonly < 8) only[nonly++] = info->activeActionSets[k].subactionPath;
            }
        if (all) nonly = 0;
        for (size_t k = 0; k < set->nactions; k++) {
            struct action *a = set->actions[k];
            for (uint32_t j = 0; j <= a->nsub; j++) {
                const XrPath sub = j == 0 ? XR_NULL_PATH : a->sub[j - 1];
                bool on = active && focused;
                if (on && nonly > 0 && sub != XR_NULL_PATH) {
                    on = false;
                    for (uint32_t m = 0; m < nonly; m++) on |= only[m] == sub;
                }
                struct action_state st = on ? evaluate(a, sub, &t) : (struct action_state){ 0 };
                struct action_state *old = &a->state[j];
                st.changed = old->active && st.active && (st.b != old->b || st.f != old->f || st.v.x != old->v.x || st.v.y != old->v.y);
                st.changed_at = st.changed || (st.active && !old->active) ? now : old->changed_at;
                *old = st;
            }
        }
    }
    return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

// The action and its state for a subaction path, or the error OpenXR gives.
static XrResult state_of(XrSession session, const XrActionStateGetInfo *info, XrActionType type, struct action_state **out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_ACTION_STATE_GET_INFO) return XR_ERROR_VALIDATION_FAILURE;
    struct action *a = (struct action *)find(H(info->action), MAGIC_ACTION);
    if (a == NULL) return XR_ERROR_HANDLE_INVALID;
    if (a->type != type) return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (!attached_to(s, a->set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (info->subactionPath == XR_NULL_PATH) { *out = &a->state[0]; return XR_SUCCESS; }
    for (uint32_t i = 0; i < a->nsub; i++)
        if (a->sub[i] == info->subactionPath) { *out = &a->state[i + 1]; return XR_SUCCESS; }
    return path_string(info->subactionPath) != NULL ? XR_ERROR_PATH_UNSUPPORTED : XR_ERROR_PATH_INVALID;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetActionStateBoolean(XrSession session, const XrActionStateGetInfo *info, XrActionStateBoolean *out)
{
    struct action_state *st;
    const XrResult r = state_of(session, info, XR_ACTION_TYPE_BOOLEAN_INPUT, &st);
    if (r != XR_SUCCESS) return r;
    if (out == NULL || out->type != XR_TYPE_ACTION_STATE_BOOLEAN) return XR_ERROR_VALIDATION_FAILURE;
    out->currentState = st->b;
    out->changedSinceLastSync = st->changed;
    out->lastChangeTime = st->changed_at;
    out->isActive = st->active;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetActionStateFloat(XrSession session, const XrActionStateGetInfo *info, XrActionStateFloat *out)
{
    struct action_state *st;
    const XrResult r = state_of(session, info, XR_ACTION_TYPE_FLOAT_INPUT, &st);
    if (r != XR_SUCCESS) return r;
    if (out == NULL || out->type != XR_TYPE_ACTION_STATE_FLOAT) return XR_ERROR_VALIDATION_FAILURE;
    out->currentState = st->f;
    out->changedSinceLastSync = st->changed;
    out->lastChangeTime = st->changed_at;
    out->isActive = st->active;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetActionStateVector2f(XrSession session, const XrActionStateGetInfo *info, XrActionStateVector2f *out)
{
    struct action_state *st;
    const XrResult r = state_of(session, info, XR_ACTION_TYPE_VECTOR2F_INPUT, &st);
    if (r != XR_SUCCESS) return r;
    if (out == NULL || out->type != XR_TYPE_ACTION_STATE_VECTOR2F) return XR_ERROR_VALIDATION_FAILURE;
    out->currentState = st->v;
    out->changedSinceLastSync = st->changed;
    out->lastChangeTime = st->changed_at;
    out->isActive = st->active;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetActionStatePose(XrSession session, const XrActionStateGetInfo *info, XrActionStatePose *out)
{
    struct action_state *st;
    const XrResult r = state_of(session, info, XR_ACTION_TYPE_POSE_INPUT, &st);
    if (r != XR_SUCCESS) return r;
    if (out == NULL || out->type != XR_TYPE_ACTION_STATE_POSE) return XR_ERROR_VALIDATION_FAILURE;
    out->isActive = st->active;
    return XR_SUCCESS;
}

// THE POSE an action space follows: the first binding of the action that names a pose, on the hand the
// subaction path names. Live, not from the last sync: OpenXR locates spaces at the time asked for.
bool input_pose(struct session *s, struct action *a, XrPath sub, struct oxs_pose *out)
{
    const char *subs = sub != XR_NULL_PATH ? path_string(sub) : NULL;
    const struct oxs_tracking t = session_tracking(s);
    if (!attached_to(s, a->set)) return false;
    for (uint32_t i = 0; i < a->nbound; i++) {
        const char *bp = path_string(a->bound[i]);
        struct source src;
        if (bp == NULL || (subs != NULL && strncmp(bp, subs, strlen(subs)) != 0) || !parse_source(a->bound[i], &src)) continue;
        if (!is(src.comp, "pose")) continue;
        if (is(src.id, "pinch_ext") || is(src.id, "poke_ext")) {
            struct oxs_pose pinch, poke;
            hand_pinch_poke(src.hand, &t.hand[src.hand].grip, &pinch, &poke);
            *out = is(src.id, "pinch_ext") ? pinch : poke;
        } else {
            *out = is(src.id, "aim") ? t.hand[src.hand].aim : t.hand[src.hand].grip;
        }
        return true;
    }
    return false;
}

// ---- haptics -------------------------------------------------------------------------------------

static XrResult haptic(XrSession session, const XrHapticActionInfo *info, float amplitude, XrDuration duration)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_HAPTIC_ACTION_INFO) return XR_ERROR_VALIDATION_FAILURE;
    struct action *a = (struct action *)find(H(info->action), MAGIC_ACTION);
    if (a == NULL) return XR_ERROR_HANDLE_INVALID;
    if (a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (!attached_to(s, a->set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (s->state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
    const char *subs = info->subactionPath != XR_NULL_PATH ? path_string(info->subactionPath) : NULL;
    // A pulse too short to see is shown for a tenth of a second.
    const XrTime until = now_ns() + (duration > 100000000 ? duration : 100000000);
    for (uint32_t i = 0; i < a->nbound; i++) {
        const char *bp = path_string(a->bound[i]);
        struct source src;
        if (bp == NULL || (subs != NULL && strncmp(bp, subs, strlen(subs)) != 0) || !parse_source(a->bound[i], &src) || !src.output)
            continue;
        s->haptic[src.hand] = amplitude;
        s->haptic_until[src.hand] = amplitude > 0 ? until : 0;
    }
    session_status(s);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_ApplyHapticFeedback(XrSession session, const XrHapticActionInfo *info,
                                                             const XrHapticBaseHeader *feedback)
{
    if (feedback == NULL) return XR_ERROR_VALIDATION_FAILURE;
    if (feedback->type != XR_TYPE_HAPTIC_VIBRATION) return haptic(session, info, 0, 0);   // PCM and others: accepted
    const XrHapticVibration *v = (const XrHapticVibration *)feedback;
    const float amplitude = v->amplitude < 0 ? 0 : v->amplitude > 1 ? 1 : v->amplitude;
    return haptic(session, info, amplitude, v->duration);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_StopHapticFeedback(XrSession session, const XrHapticActionInfo *info)
{
    return haptic(session, info, 0, 0);
}

// ---- describing sources --------------------------------------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo *info,
                                                                        uint32_t cap, uint32_t *count, XrPath *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    struct action *a = (struct action *)find(H(info->action), MAGIC_ACTION);
    if (a == NULL) return XR_ERROR_HANDLE_INVALID;
    if (!attached_to(s, a->set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    TWO_CALL(cap, count, out, a->bound, a->nbound);
    return XR_SUCCESS;
}

static void capitalised(char *dst, size_t n, const char *word)
{
    snprintf(dst, n, "%s", word);
    const size_t len = strlen(dst);   // "pinch_ext" is the Pinch: the suffix is the extension's, not a name
    if (len > 4 && strcmp(dst + len - 4, "_ext") == 0) dst[len - 4] = '\0';
    if (dst[0] >= 'a' && dst[0] <= 'z') dst[0] = (char)(dst[0] - 'a' + 'A');
    for (char *c = dst; *c; c++) if (*c == '_') *c = ' ';
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo *info,
                                                                     uint32_t cap, uint32_t *count, char *buffer)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO || info->whichComponents == 0 || count == NULL)
        return XR_ERROR_VALIDATION_FAILURE;
    struct source src;
    if (!parse_source(info->sourcePath, &src)) return XR_ERROR_PATH_UNSUPPORTED;
    char name[160] = "", part[64];
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT)
        snprintf(name + strlen(name), sizeof name - strlen(name), "%s", src.hand == 0 ? "Left Hand" : "Right Hand");
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT)
        snprintf(name + strlen(name), sizeof name - strlen(name), "%sSimulated Controller", name[0] ? " " : "");
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT) {
        capitalised(part, sizeof part, src.id);
        snprintf(name + strlen(name), sizeof name - strlen(name), "%s%s", name[0] ? " " : "", part);
    }
    const uint32_t n = (uint32_t)strlen(name) + 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n || buffer == NULL) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, name, n);
    return XR_SUCCESS;
}

const struct entry input_functions[] = {
    ENTRY(CreateActionSet) ENTRY(DestroyActionSet) ENTRY(CreateAction) ENTRY(DestroyAction)
    ENTRY(SuggestInteractionProfileBindings) ENTRY(AttachSessionActionSets) ENTRY(GetCurrentInteractionProfile)
    ENTRY(SyncActions) ENTRY(GetActionStateBoolean) ENTRY(GetActionStateFloat) ENTRY(GetActionStateVector2f)
    ENTRY(GetActionStatePose) ENTRY(ApplyHapticFeedback) ENTRY(StopHapticFeedback)
    ENTRY(EnumerateBoundSourcesForAction) ENTRY(GetInputSourceLocalizedName)
    { NULL, NULL }
};
