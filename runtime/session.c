// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// THE SESSION: its states, its frames, its spaces and views (DESIGN.md).
//
// STATES. Created: IDLE, then READY at once -- the headset is always there to be used. xrBeginSession
// takes it through SYNCHRONIZED and VISIBLE to FOCUSED; there is no display to synchronise with and no
// other application to take the focus, so nothing holds it back. Leaving goes the other way down to
// STOPPING (xrRequestExitSession, or the person closing the simulator's window), xrEndSession to IDLE and
// then EXITING: a closed window is the headset taken off for good, and there is no way to put it back on
// but to start the session again.
//
// FRAMES. xrWaitFrame paces the application on its own clock at 72 Hz (DESIGN.md: the simulator is not a
// display, and a hidden window must not stop the application) and blocks while a frame it handed out has
// not been begun, as OpenXR allows. xrEndFrame hands the last projection layer's two eyes to the device.
#include "internal.h"
#include "xrmath.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_RATE 72
#define IPD 0.063f
#define HALF_FOV 0.7853982f   // 45 degrees each way: 90 across, square (DESIGN.md)

static const char *state_name(XrSessionState s)
{
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "idle";
        case XR_SESSION_STATE_READY: return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE: return "visible";
        case XR_SESSION_STATE_FOCUSED: return "focused";
        case XR_SESSION_STATE_STOPPING: return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
        case XR_SESSION_STATE_EXITING: return "exiting";
        default: return "unknown";
    }
}

void session_status(struct session *s)
{
    if (!s->device_open || device()->status == NULL) return;
    const XrTime t = now_ns();
    struct oxs_status st = {
        .app = s->in->app_name,
        .profile = s->profile != XR_NULL_PATH ? path_string(s->profile) : "",
        .state = (int)s->state,
        .fps = s->fps,
    };
    for (int h = 0; h < 2; h++) st.haptic[h] = t < s->haptic_until[h] ? s->haptic[h] : 0;
    device()->status(&st);
}

static void set_state(struct session *s, XrSessionState state)
{
    s->state = state;
    const XrEventDataSessionStateChanged e = {
        .type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,
        .session = (XrSession)(uintptr_t)s,
        .state = state,
        .time = now_ns(),
    };
    post_event(s->in, &e, sizeof e);
    say("session: %s", state_name(state));
    session_status(s);
}

// THE WINDOW CLOSED: the session steps down to STOPPING, as for xrRequestExitSession.
static void begin_exit(struct session *s)
{
    if (s->exit_requested) return;
    s->exit_requested = true;
    if (!s->running) {   // never begun: nothing to stop, only to leave
        if (s->state == XR_SESSION_STATE_READY) set_state(s, XR_SESSION_STATE_IDLE);
        set_state(s, XR_SESSION_STATE_EXITING);
        return;
    }
    if (s->state == XR_SESSION_STATE_FOCUSED) set_state(s, XR_SESSION_STATE_VISIBLE);
    if (s->state == XR_SESSION_STATE_VISIBLE) set_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    set_state(s, XR_SESSION_STATE_STOPPING);
}

struct oxs_tracking session_tracking(struct session *s)
{
    struct oxs_tracking t;
    pthread_mutex_lock(&s->track_lock);
    // A window that has gone leaves the last poses where they were; session_poll ends the session.
    if (s->device_open && device()->track != NULL && device()->track(&t)) s->tracking = t;
    t = s->tracking;
    pthread_mutex_unlock(&s->track_lock);
    return t;
}

void session_poll(struct session *s)
{
    if (!s->device_open || s->exit_requested || device()->track == NULL) return;
    struct oxs_tracking t;
    pthread_mutex_lock(&s->track_lock);
    const bool there = device()->track(&t);
    if (there) s->tracking = t;
    pthread_mutex_unlock(&s->track_lock);
    if (!there) begin_exit(s);
}

// LOCAL: where the head was, turned only about the vertical, at its height (OpenXR: gravity-aligned).
static struct oxs_pose level(const struct oxs_pose *head)
{
    float fwd[3];
    oxs_quat_rotate(head->q, (const float[3]){ 0, 0, -1 }, fwd);
    struct oxs_pose p = oxs_pose_identity();
    oxs_quat_from_euler(atan2f(-fwd[0], -fwd[2]), 0, 0, p.q);
    memcpy(p.p, head->p, sizeof p.p);
    return p;
}

// ---- the session ---------------------------------------------------------------------------------

static void session_fini(struct object *o)
{
    struct session *s = (struct session *)o;
    if (s->device_open && device()->close != NULL) device()->close();
    s->device_open = false;
    if (s->gfx == GFX_VULKAN) vk_session_fini(s);
    if (s->gfx == GFX_GL) gl_session_fini(s);
#ifdef _WIN32
    if (s->gfx == GFX_D3D11) d3d11_session_fini(s);
    if (s->gfx == GFX_D3D12) d3d12_session_fini(s);
#endif
    if (s->in->session == s) s->in->session = NULL;
    for (size_t i = 0; i < s->nattached; i++) s->attached[i]->attached = false;
    free(s->attached);
    pthread_mutex_destroy(&s->frame_lock);
    pthread_cond_destroy(&s->frame_cond);
    pthread_mutex_destroy(&s->track_lock);
}

// The graphics binding, in the create info's chain.
static const XrBaseInStructure *binding_of(const XrSessionCreateInfo *info)
{
    for (const XrBaseInStructure *b = info->next; b != NULL; b = b->next)
        switch ((int)b->type) {
            case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR:
            case XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR:
            case XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR:
            case XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR:
            case XR_TYPE_GRAPHICS_BINDING_EGL_MNDX:
            // A Windows program's, under Wine: the context behind WGL is a host one (swapchain_gl.c).
            case XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR:
            case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR:
            case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR:
                return b;
            default: break;
        }
    return NULL;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *out)
{
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_SESSION_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (!system_ok(in, info->systemId)) return XR_ERROR_SYSTEM_INVALID;
    if (in->session != NULL) return XR_ERROR_LIMIT_REACHED;
    const XrBaseInStructure *b = binding_of(info);
    enum graphics gfx = GFX_NONE;
    if (b == NULL) {
        // SAID, not only returned: an application's log shows the code and nothing of why.
        if (!in->headless) {
            fprintf(stderr, "openxr-sim: xrCreateSession: no graphics binding this runtime knows (the chain starts with type %d)\n",
                    info->next != NULL ? (int)((const XrBaseInStructure *)info->next)->type : 0);
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
    } else if (b->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        if (!in->asked_vulkan) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        gfx = GFX_VULKAN;
    } else if (b->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || b->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
#ifdef _WIN32
        const bool eleven = b->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
        if (!(eleven ? in->asked_d3d11 : in->asked_d3d12)) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        gfx = eleven ? GFX_D3D11 : GFX_D3D12;
#else
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;   // a Direct3D binding reaching a Linux runtime
#endif
    } else {
        if (!in->asked_gl) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        gfx = GFX_GL;
    }

    struct session *s = make(sizeof *s, MAGIC_SESSION, &in->o, NULL);
    if (s == NULL) return XR_ERROR_OUT_OF_MEMORY;
    s->in = in;
    pthread_mutex_init(&s->frame_lock, NULL);
    pthread_cond_init(&s->frame_cond, NULL);
    pthread_mutex_init(&s->track_lock, NULL);
    s->o.fini = session_fini;
    s->frame_period = 1000000000 / FRAME_RATE;
    s->fps_since = now_ns();
    oxs_tracking_home(&s->tracking);
    s->gfx = gfx;
    XrResult r = XR_SUCCESS;
    if (gfx == GFX_VULKAN) r = vk_session_init(s, (const XrGraphicsBindingVulkanKHR *)b);
    if (gfx == GFX_GL) r = gl_session_init(s, b);
#ifdef _WIN32
    if (gfx == GFX_D3D11) r = d3d11_session_init(s, (const XrGraphicsBindingD3D11KHR *)b);
    if (gfx == GFX_D3D12) r = d3d12_session_init(s, (const XrGraphicsBindingD3D12KHR *)b);
#endif
    if (r != XR_SUCCESS) {
        s->gfx = GFX_NONE;
        destroy(&s->o);
        return r;
    }
    // THE WINDOW, unless the application asked for no display at all.
    if (gfx != GFX_NONE && device()->open != NULL) {
        char why[256] = "";
        if (!device()->open(in->app_name, why, sizeof why)) {
            fprintf(stderr, "openxr-sim: the simulated headset cannot be shown: %s\n", why);
            destroy(&s->o);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        s->device_open = true;
    }
    s->local_origin = level(&s->tracking.head);
    in->session = s;
    *out = (XrSession)(uintptr_t)s;
    set_state(s, XR_SESSION_STATE_IDLE);
    set_state(s, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroySession(XrSession session)
{
    struct object *o = find(H(session), MAGIC_SESSION);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_BeginSession(XrSession session, const XrSessionBeginInfo *info)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_SESSION_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->running) return XR_ERROR_SESSION_RUNNING;
    if (s->state != XR_SESSION_STATE_READY) return XR_ERROR_SESSION_NOT_READY;
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    pthread_mutex_lock(&s->frame_lock);
    s->running = true;
    s->waited = s->begun = s->ended = 0;
    s->last_display = 0;
    pthread_mutex_unlock(&s->frame_lock);
    set_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    set_state(s, XR_SESSION_STATE_VISIBLE);
    set_state(s, XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EndSession(XrSession session)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != XR_SESSION_STATE_STOPPING) return XR_ERROR_SESSION_NOT_STOPPING;
    pthread_mutex_lock(&s->frame_lock);
    s->running = false;
    pthread_cond_broadcast(&s->frame_cond);
    pthread_mutex_unlock(&s->frame_lock);
    set_state(s, XR_SESSION_STATE_IDLE);
    set_state(s, XR_SESSION_STATE_EXITING);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_RequestExitSession(XrSession session)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    begin_exit(s);
    return XR_SUCCESS;
}

// ---- frames --------------------------------------------------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL oxs_WaitFrame(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if ((info != NULL && info->type != XR_TYPE_FRAME_WAIT_INFO) || state == NULL || state->type != XR_TYPE_FRAME_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    session_poll(s);
    pthread_mutex_lock(&s->frame_lock);
    while (s->running && s->waited > s->begun) pthread_cond_wait(&s->frame_cond, &s->frame_lock);
    if (!s->running) { pthread_mutex_unlock(&s->frame_lock); return XR_ERROR_SESSION_NOT_RUNNING; }
    // THE NEXT DISPLAY TIME: one period after the last, or the next one ahead of now if the application
    // fell behind; the wait ends a period before it, when the frame should start.
    const XrTime now = now_ns(), period = s->frame_period;
    XrTime display = s->last_display != 0 ? s->last_display + period : now + period;
    if (display < now + period / 2) display = now + period;
    s->last_display = display;
    s->waited++;
    pthread_mutex_unlock(&s->frame_lock);
    const XrTime wake = display - period;
    plat_sleep_until_ns(wake);
    state->predictedDisplayTime = display;
    state->predictedDisplayPeriod = period;
    state->shouldRender = s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_BeginFrame(XrSession session, const XrFrameBeginInfo *info)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info != NULL && info->type != XR_TYPE_FRAME_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    pthread_mutex_lock(&s->frame_lock);
    XrResult r = XR_SUCCESS;
    if (s->begun >= s->waited) r = XR_ERROR_CALL_ORDER_INVALID;
    else {
        if (s->begun > s->ended) { s->ended = s->begun; r = XR_FRAME_DISCARDED; }
        s->begun++;
        pthread_cond_broadcast(&s->frame_cond);
    }
    pthread_mutex_unlock(&s->frame_lock);
    return r;
}

static struct swapchain *released(const XrSwapchainSubImage *sub)
{
    struct swapchain *sc = (struct swapchain *)find(H(sub->swapchain), MAGIC_SWAPCHAIN);
    return sc != NULL && sc->last_released >= 0 ? sc : NULL;
}

static struct oxs_pose from_xr(const XrPosef *p);

// A layer's sub-image: the rectangle of the image, cut to the image.
static void image_of(const XrSwapchainSubImage *sub, struct oxs_eye *out)
{
    memset(out, 0, sizeof *out);
    struct swapchain *sc = released(sub);
    if (sc == NULL || !sc->color || sc->pixels[sc->last_released] == NULL) return;
    const uint32_t w = sc->info.width, h = sc->info.height;
    int32_t x = sub->imageRect.offset.x, y = sub->imageRect.offset.y;
    int32_t rw = sub->imageRect.extent.width, rh = sub->imageRect.extent.height;
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > (int32_t)w) rw = (int32_t)w - x;
    if (y + rh > (int32_t)h) rh = (int32_t)h - y;
    if (rw <= 0 || rh <= 0 || sub->imageArrayIndex >= sc->info.arraySize) return;
    const uint8_t *layer = sc->pixels[sc->last_released] + (size_t)sub->imageArrayIndex * w * h * 4;
    out->pixels = layer + ((size_t)y * w + (size_t)x) * 4;
    out->width = (uint32_t)rw;
    out->height = (uint32_t)rh;
    out->stride = w * 4;
    out->bgra = sc->bgra;
    out->linear = sc->linear;
    out->flipped = sc->gl_flipped;
}

static void eye_of(const XrCompositionLayerProjectionView *v, struct oxs_eye *out) { image_of(&v->subImage, out); }

static XrResult check_sub(const XrSwapchainSubImage *sub)
{
    if (find(H(sub->swapchain), MAGIC_SWAPCHAIN) == NULL) return XR_ERROR_HANDLE_INVALID;
    return released(sub) != NULL ? XR_SUCCESS : XR_ERROR_LAYER_INVALID;
}

static bool visibility_ok(XrEyeVisibility v)
{
    return v == XR_EYE_VISIBILITY_BOTH || v == XR_EYE_VISIBILITY_LEFT || v == XR_EYE_VISIBILITY_RIGHT;
}

// A layer of a type this runtime draws, whole: an unknown type, or one whose extension was not enabled,
// is XR_ERROR_LAYER_INVALID, as the specification has it -- not silently left out of the picture.
static XrResult check_layer(struct session *s, const XrCompositionLayerBaseHeader *l)
{
    if (l == NULL) return XR_ERROR_LAYER_INVALID;
    if (find(H(l->space), MAGIC_SPACE) == NULL) return XR_ERROR_HANDLE_INVALID;
    XrResult r = XR_SUCCESS;
    switch (l->type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
            const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
            if (p->viewCount != 2 || p->views == NULL) return XR_ERROR_VALIDATION_FAILURE;
            for (int e = 0; e < 2 && r == XR_SUCCESS; e++) r = check_sub(&p->views[e].subImage);
            return r;
        }
        case XR_TYPE_COMPOSITION_LAYER_QUAD: {
            const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)l;
            if (!visibility_ok(q->eyeVisibility)) return XR_ERROR_VALIDATION_FAILURE;
            return check_sub(&q->subImage);
        }
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
            const XrCompositionLayerCylinderKHR *c = (const XrCompositionLayerCylinderKHR *)l;
            if (!s->in->cylinder) return XR_ERROR_LAYER_INVALID;
            if (!visibility_ok(c->eyeVisibility) || !(c->radius >= 0) || !(c->centralAngle >= 0) ||
                c->centralAngle >= 2 * (float)M_PI || !(c->aspectRatio > 0))
                return XR_ERROR_VALIDATION_FAILURE;
            return check_sub(&c->subImage);
        }
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
            const XrCompositionLayerEquirect2KHR *q = (const XrCompositionLayerEquirect2KHR *)l;
            if (!s->in->equirect2) return XR_ERROR_LAYER_INVALID;
            if (!visibility_ok(q->eyeVisibility) || !(q->radius >= 0) || !(q->centralHorizontalAngle >= 0) ||
                q->centralHorizontalAngle > 2 * (float)M_PI + 1e-4f)
                return XR_ERROR_VALIDATION_FAILURE;
            return check_sub(&q->subImage);
        }
        default: return XR_ERROR_LAYER_INVALID;
    }
}

static const XrCompositionLayerColorScaleBiasKHR *scale_bias_of(struct session *s, const XrCompositionLayerBaseHeader *l)
{
    if (!s->in->color_scale_bias) return NULL;
    for (const XrBaseInStructure *n = l->next; n != NULL; n = n->next)
        if (n->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) return (const XrCompositionLayerColorScaleBiasKHR *)n;
    return NULL;
}

static void field_of(const XrFovf *f, float *l, float *r, float *u, float *d)
{
    *l = tanf(f->angleLeft);
    *r = tanf(f->angleRight);
    *u = tanf(f->angleUp);
    *d = tanf(f->angleDown);
}

// THE EYE A FRAME IS COMPOSITED INTO (by the simulator, src/composite.c): seen through the first projection
// layer's views, at their size -- the picture the application drew, which every other layer is placed
// against. A frame with no projection layer, or one in a space that is not tracked, is seen from the head
// as xrLocateViews puts it, at the recommended size.
static struct oxs_camera eye_camera(struct session *s, const XrFrameEndInfo *info, const XrCompositionLayerProjection *base, int e)
{
    struct oxs_camera cam = { 0 };
    struct oxs_pose sp;
    if (base != NULL && space_in_stage((struct space *)find(H(base->space), MAGIC_SPACE), info->displayTime, &sp)) {
        struct oxs_eye im;
        eye_of(&base->views[e], &im);
        if (im.pixels != NULL) {
            const struct oxs_pose view = from_xr(&base->views[e].pose);
            cam.pose = oxs_pose_compose(&sp, &view);
            field_of(&base->views[e].fov, &cam.tan_left, &cam.tan_right, &cam.tan_up, &cam.tan_down);
            cam.width = im.width;
            cam.height = im.height;
        }
    }
    if (cam.width == 0) {
        struct oxs_pose eye = oxs_pose_identity();
        eye.p[0] = e == 0 ? -IPD / 2 : IPD / 2;
        const struct oxs_pose head = session_tracking(s).head;
        cam.pose = oxs_pose_compose(&head, &eye);
        cam.tan_left = cam.tan_down = -tanf(HALF_FOV);
        cam.tan_right = cam.tan_up = tanf(HALF_FOV);
        cam.width = cam.height = EYE_RECOMMENDED;
    }
    return cam;
}

// One submitted layer as the simulator composites it for eye `e`; false when it has nothing to show that eye.
static bool comp_layer_of(struct session *s, const XrFrameEndInfo *info, const XrCompositionLayerBaseHeader *l,
                          const XrCompositionLayerProjection *base, int e, struct oxs_layer *out)
{
    memset(out, 0, sizeof *out);
    struct oxs_pose sp;
    if (!space_in_stage((struct space *)find(H(l->space), MAGIC_SPACE), info->displayTime, &sp)) return false;
    XrEyeVisibility vis = XR_EYE_VISIBILITY_BOTH;
    XrPosef pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
    const XrSwapchainSubImage *sub = NULL;
    switch (l->type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
            const XrCompositionLayerProjectionView *v = &((const XrCompositionLayerProjection *)l)->views[e];
            out->kind = OXS_PROJECTION;
            pose = v->pose;
            field_of(&v->fov, &out->tan_left, &out->tan_right, &out->tan_up, &out->tan_down);
            out->own_view = l == (const XrCompositionLayerBaseHeader *)base;
            sub = &v->subImage;
            break;
        }
        case XR_TYPE_COMPOSITION_LAYER_QUAD: {
            const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)l;
            out->kind = OXS_QUAD;
            vis = q->eyeVisibility;
            pose = q->pose;
            out->size[0] = q->size.width;
            out->size[1] = q->size.height;
            sub = &q->subImage;
            break;
        }
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
            const XrCompositionLayerCylinderKHR *c = (const XrCompositionLayerCylinderKHR *)l;
            out->kind = OXS_CYLINDER;
            vis = c->eyeVisibility;
            pose = c->pose;
            out->radius = c->radius;
            out->angle = c->centralAngle;
            out->aspect = c->aspectRatio;
            sub = &c->subImage;
            break;
        }
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
            const XrCompositionLayerEquirect2KHR *q = (const XrCompositionLayerEquirect2KHR *)l;
            out->kind = OXS_EQUIRECT;
            vis = q->eyeVisibility;
            pose = q->pose;
            out->radius = q->radius;
            out->angle = q->centralHorizontalAngle;
            out->upper = q->upperVerticalAngle;
            out->lower = q->lowerVerticalAngle;
            sub = &q->subImage;
            break;
        }
        default: return false;
    }
    if (vis == XR_EYE_VISIBILITY_LEFT && e != 0) return false;
    if (vis == XR_EYE_VISIBILITY_RIGHT && e != 1) return false;
    const struct oxs_pose local = from_xr(&pose);
    out->pose = oxs_pose_compose(&sp, &local);
    out->source_alpha = (l->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
    out->unpremultiplied = (l->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
    const XrCompositionLayerColorScaleBiasKHR *sb = scale_bias_of(s, l);
    if (sb != NULL) {
        out->scale_bias = true;
        const float sc[4] = { sb->colorScale.r, sb->colorScale.g, sb->colorScale.b, sb->colorScale.a };
        const float bi[4] = { sb->colorBias.r, sb->colorBias.g, sb->colorBias.b, sb->colorBias.a };
        memcpy(out->scale, sc, sizeof sc);
        memcpy(out->bias, bi, sizeof bi);
    }
    image_of(sub, &out->image);
    return out->image.pixels != NULL;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EndFrame(XrSession session, const XrFrameEndInfo *info)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_FRAME_END_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (info->displayTime <= 0) return XR_ERROR_TIME_INVALID;
    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    if (info->layerCount > XR_MIN_COMPOSITION_LAYERS_SUPPORTED) return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    if (info->layerCount > 0 && info->layers == NULL) return XR_ERROR_VALIDATION_FAILURE;
    const XrCompositionLayerProjection *base = NULL;   // the first projection: the eyes are seen through it
    for (uint32_t i = 0; i < info->layerCount; i++) {
        const XrResult r = check_layer(s, info->layers[i]);
        if (r != XR_SUCCESS) return r;
        if (base == NULL && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            base = (const XrCompositionLayerProjection *)info->layers[i];
    }
    pthread_mutex_lock(&s->frame_lock);
    XrResult r = XR_SUCCESS;
    if (s->ended >= s->begun) r = XR_ERROR_CALL_ORDER_INVALID;
    else s->ended = s->begun;
    pthread_mutex_unlock(&s->frame_lock);
    if (r != XR_SUCCESS) return r;

    if (info->layerCount > 0 && s->device_open && device()->present != NULL) {
        struct oxs_frame f = { .display_time = info->displayTime, .number = s->ended };
        // ONE PROJECTION LAYER, as nearly every frame is: its eyes go on as they are. Over nothing, an
        // opaque or premultiplied layer is its own colour; only an unpremultiplied one with its alpha
        // enabled, or a colour scale, changes it.
        const XrCompositionLayerBaseHeader *l0 = info->layers[0];
        const bool plain = info->layerCount == 1 && base != NULL && scale_bias_of(s, l0) == NULL &&
                           (l0->layerFlags & (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)) !=
                               (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
        for (int e = 0; e < 2; e++) {
            if (base != NULL) eye_of(&base->views[e], &f.eye[e]);
            if (plain) continue;
            // Anything else the simulator composites: every layer, for each eye, in the order submitted.
            f.camera[e] = eye_camera(s, info, base, e);
            for (uint32_t i = 0; i < info->layerCount; i++)
                if (comp_layer_of(s, info, info->layers[i], base, e, &f.layer[e][f.nlayers[e]])) f.nlayers[e]++;
        }
        if (!device()->present(&f)) begin_exit(s);
    }
    // THE FRAME RATE the simulator shows, over the last second.
    s->fps_frames++;
    const XrTime t = now_ns();
    if (t - s->fps_since >= 1000000000) {
        s->fps = (float)s->fps_frames * 1e9f / (float)(t - s->fps_since);
        s->fps_frames = 0;
        s->fps_since = t;
    }
    session_status(s);
    return XR_SUCCESS;
}

// ---- spaces --------------------------------------------------------------------------------------

static bool pose_ok(const XrPosef *p)
{
    const float q[4] = { p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w };
    return oxs_quat_valid(q);
}

static struct oxs_pose from_xr(const XrPosef *p)
{
    struct oxs_pose r = { { p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w },
                             { p->position.x, p->position.y, p->position.z } };
    oxs_quat_normalize(r.q);
    return r;
}

static XrPosef to_xr(const struct oxs_pose *p)
{
    return (XrPosef){ { p->q[0], p->q[1], p->q[2], p->q[3] }, { p->p[0], p->p[1], p->p[2] } };
}

static const XrReferenceSpaceType REF_SPACES[] = {
    XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE,
    XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
};

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateReferenceSpaces(XrSession session, uint32_t cap, uint32_t *count,
                                                                  XrReferenceSpaceType *out)
{
    if (find(H(session), MAGIC_SESSION) == NULL) return XR_ERROR_HANDLE_INVALID;
    TWO_CALL(cap, count, out, REF_SPACES, sizeof REF_SPACES / sizeof REF_SPACES[0]);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *info, XrSpace *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    bool known = false;
    for (size_t i = 0; i < sizeof REF_SPACES / sizeof REF_SPACES[0]; i++) known |= REF_SPACES[i] == info->referenceSpaceType;
    if (!known) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    if (!pose_ok(&info->poseInReferenceSpace)) return XR_ERROR_POSE_INVALID;
    struct space *sp = make(sizeof *sp, MAGIC_SPACE, &s->in->o, &s->o);
    if (sp == NULL) return XR_ERROR_OUT_OF_MEMORY;
    sp->session = s;
    sp->ref = info->referenceSpaceType;
    sp->offset = from_xr(&info->poseInReferenceSpace);
    *out = (XrSpace)(uintptr_t)sp;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType type, XrExtent2Df *bounds)
{
    if (find(H(session), MAGIC_SESSION) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (bounds == NULL) return XR_ERROR_VALIDATION_FAILURE;
    bool known = false;
    for (size_t i = 0; i < sizeof REF_SPACES / sizeof REF_SPACES[0]; i++) known |= REF_SPACES[i] == type;
    if (!known) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    // A 3 m square play area, for applications that draw its edge; the others have none.
    if (type == XR_REFERENCE_SPACE_TYPE_STAGE) { bounds->width = bounds->height = 3.0f; return XR_SUCCESS; }
    bounds->width = bounds->height = 0;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *info, XrSpace *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    struct action *a = (struct action *)find(H(info->action), MAGIC_ACTION);
    if (a == NULL) return XR_ERROR_HANDLE_INVALID;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (info->subactionPath != XR_NULL_PATH) {
        bool listed = false;
        for (uint32_t i = 0; i < a->nsub; i++) listed |= a->sub[i] == info->subactionPath;
        if (!listed) return path_string(info->subactionPath) != NULL ? XR_ERROR_PATH_UNSUPPORTED : XR_ERROR_PATH_INVALID;
    }
    if (!pose_ok(&info->poseInActionSpace)) return XR_ERROR_POSE_INVALID;
    struct space *sp = make(sizeof *sp, MAGIC_SPACE, &s->in->o, &s->o);
    if (sp == NULL) return XR_ERROR_OUT_OF_MEMORY;
    sp->session = s;
    sp->action = a;
    sp->subaction = info->subactionPath;
    sp->offset = from_xr(&info->poseInActionSpace);
    *out = (XrSpace)(uintptr_t)sp;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroySpace(XrSpace space)
{
    struct object *o = find(H(space), MAGIC_SPACE);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

bool space_in_stage(struct space *sp, XrTime t, struct oxs_pose *out)
{
    (void)t;   // no prediction: nothing simulated moves on its own between two reads
    struct session *s = sp->session;
    struct oxs_pose base = oxs_pose_identity();
    if (sp->action != NULL) {
        // An action that was destroyed takes its spaces' meaning with it.
        if (find(H(sp->action), MAGIC_ACTION) == NULL || !input_pose(s, sp->action, sp->subaction, &base)) return false;
    } else {
        switch (sp->ref) {
            case XR_REFERENCE_SPACE_TYPE_VIEW: base = session_tracking(s).head; break;
            case XR_REFERENCE_SPACE_TYPE_LOCAL: base = s->local_origin; break;
            case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: base = s->local_origin; base.p[1] = 0; break;
            default: break;   // STAGE is the frame everything is kept in
        }
    }
    *out = oxs_pose_compose(&base, &sp->offset);
    return true;
}

static void locate(struct space *sp, struct space *base, XrTime t, XrSpaceLocationFlags *flags, XrPosef *pose)
{
    struct oxs_pose a, b;
    if (!space_in_stage(sp, t, &a) || !space_in_stage(base, t, &b)) {
        *flags = 0;
        *pose = (XrPosef){ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        return;
    }
    const struct oxs_pose r = oxs_pose_relative(&b, &a);
    *pose = to_xr(&r);
    *flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
             XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
}

// Velocities are asked for in the chain; nothing is reported moving.
static void no_velocity(void *next)
{
    for (XrBaseOutStructure *n = next; n != NULL; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITY) {
            XrSpaceVelocity *v = (XrSpaceVelocity *)n;
            v->velocityFlags = 0;
            v->linearVelocity = (XrVector3f){ 0, 0, 0 };
            v->angularVelocity = (XrVector3f){ 0, 0, 0 };
        }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_LocateSpace(XrSpace space, XrSpace base, XrTime t, XrSpaceLocation *location)
{
    struct space *sp = (struct space *)find(H(space), MAGIC_SPACE), *bs = (struct space *)find(H(base), MAGIC_SPACE);
    if (sp == NULL || bs == NULL) return XR_ERROR_HANDLE_INVALID;
    if (location == NULL || location->type != XR_TYPE_SPACE_LOCATION) return XR_ERROR_VALIDATION_FAILURE;
    if (t <= 0) return XR_ERROR_TIME_INVALID;
    locate(sp, bs, t, &location->locationFlags, &location->pose);
    no_velocity(location->next);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_LocateSpaces(XrSession session, const XrSpacesLocateInfo *info, XrSpaceLocations *out)
{
    if (find(H(session), MAGIC_SESSION) == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_SPACES_LOCATE_INFO || out->type != XR_TYPE_SPACE_LOCATIONS)
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->time <= 0) return XR_ERROR_TIME_INVALID;
    if (out->locationCount != info->spaceCount || (info->spaceCount > 0 && (info->spaces == NULL || out->locations == NULL)))
        return XR_ERROR_VALIDATION_FAILURE;
    struct space *bs = (struct space *)find(H(info->baseSpace), MAGIC_SPACE);
    if (bs == NULL) return XR_ERROR_HANDLE_INVALID;
    for (uint32_t i = 0; i < info->spaceCount; i++) {
        struct space *sp = (struct space *)find(H(info->spaces[i]), MAGIC_SPACE);
        if (sp == NULL) return XR_ERROR_HANDLE_INVALID;
        locate(sp, bs, info->time, &out->locations[i].locationFlags, &out->locations[i].pose);
    }
    for (XrBaseOutStructure *n = out->next; n != NULL; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITIES) {
            XrSpaceVelocities *v = (XrSpaceVelocities *)n;
            for (uint32_t i = 0; i < v->velocityCount && v->velocities != NULL; i++)
                v->velocities[i] = (XrSpaceVelocityData){ 0 };
        }
    return XR_SUCCESS;
}

// ---- views ---------------------------------------------------------------------------------------

static XRAPI_ATTR XrResult XRAPI_CALL oxs_LocateViews(XrSession session, const XrViewLocateInfo *info, XrViewState *state,
                                                     uint32_t cap, uint32_t *count, XrView *views)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || state == NULL || count == NULL || info->type != XR_TYPE_VIEW_LOCATE_INFO ||
        state->type != XR_TYPE_VIEW_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (info->displayTime <= 0) return XR_ERROR_TIME_INVALID;
    struct space *base = (struct space *)find(H(info->space), MAGIC_SPACE);
    if (base == NULL) return XR_ERROR_HANDLE_INVALID;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2 || views == NULL) return XR_ERROR_SIZE_INSUFFICIENT;
    struct oxs_pose b;
    const bool tracked = space_in_stage(base, info->displayTime, &b);
    const struct oxs_pose head = session_tracking(s).head;
    for (int e = 0; e < 2; e++) {
        if (views[e].type != XR_TYPE_VIEW) return XR_ERROR_VALIDATION_FAILURE;
        struct oxs_pose eye = oxs_pose_identity();
        eye.p[0] = e == 0 ? -IPD / 2 : IPD / 2;
        const struct oxs_pose in_stage = oxs_pose_compose(&head, &eye);
        const struct oxs_pose rel = oxs_pose_relative(&b, &in_stage);
        views[e].pose = tracked ? to_xr(&rel) : (XrPosef){ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        views[e].fov = (XrFovf){ -HALF_FOV, HALF_FOV, HALF_FOV, -HALF_FOV };
    }
    state->viewStateFlags = tracked ? XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                      XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT
                                    : 0;
    return XR_SUCCESS;
}

const struct entry session_functions[] = {
    ENTRY(CreateSession) ENTRY(DestroySession) ENTRY(BeginSession) ENTRY(EndSession) ENTRY(RequestExitSession)
    ENTRY(WaitFrame) ENTRY(BeginFrame) ENTRY(EndFrame)
    ENTRY(EnumerateReferenceSpaces) ENTRY(CreateReferenceSpace) ENTRY(GetReferenceSpaceBoundsRect)
    ENTRY(CreateActionSpace) ENTRY(DestroySpace) ENTRY(LocateSpace) ENTRY(LocateSpaces) ENTRY(LocateViews)
    { NULL, NULL }
};
