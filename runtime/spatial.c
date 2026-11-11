// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// XR_EXT_spatial_entity with XR_EXT_spatial_marker_tracking and XR_EXT_spatial_anchor, and the
// XR_EXT_future they are asynchronous through. DESIGN.md, "Spatial entities", says what is simulated and why.
//
// MARKERS are the room's (common/device.h, struct oxs_room), which the simulator owns: an entity while the
// headset can see it (oxs_marker_seen, common/xrmath.h). ANCHORS are the application's, made in a context
// and kept by it: a pose in STAGE, which nothing simulated ever moves, so an anchor is TRACKING wherever its
// base space can be located.
//
// FUTURES are ready as soon as they are made: nothing here waits on a camera. Each is an object of its own,
// a child of the session (a context being made) or of the context (a discovery snapshot), so destroying
// either invalidates it; completing or cancelling it destroys it.
//
// A CONTEXT keeps the entities it knows: a marker it has discovered (by the marker's serial), an anchor it
// made. An id is never reused -- one counter for the process. A marker that has left the room is STOPPED
// for good, one out of view PAUSED; an anchor whose handle from xrCreateSpatialAnchorEXT was destroyed is
// STOPPED. SNAPSHOTS are copies, taken when they are made (a discovery snapshot at its completion, which
// names the base space and time), so their data never changes.
#include "internal.h"
#include "xrmath.h"

#include <stdlib.h>
#include <string.h>

enum { FUTURE_CONTEXT = 1, FUTURE_DISCOVERY };

// The components there are, as bits.
enum { C_MARKER = 1, C_BOUNDED = 2, C_ANCHOR = 4 };

static unsigned component_bit(XrSpatialComponentTypeEXT t)
{
    switch (t) {
        case XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT: return C_MARKER;
        case XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT: return C_BOUNDED;
        case XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT: return C_ANCHOR;
        default: return 0;
    }
}

// A capability as a context was configured with it.
struct capconf {
    XrSpatialCapabilityEXT cap;
    int dict;                   // ArUco, AprilTag: the one dictionary it looks for
    unsigned components;        // C_*: those enabled
    float fixed_size;           // XrSpatialMarkerSizeEXT: every marker's side, else 0
};

#define CAPS_MAX 5              // every capability once
#define ANCHORS_MAX 1024        // per context: XR_ERROR_LIMIT_REACHED after

struct future {
    struct object o;
    int kind;
    struct object *owner;       // the session or the context it was made from
    // FUTURE_CONTEXT
    struct capconf caps[CAPS_MAX];
    uint32_t ncaps;
    // FUTURE_DISCOVERY: which components' data, and the tracking state asked for (0: any)
    unsigned want;
    XrSpatialEntityTrackingStateEXT filter;
};

struct known {
    XrSpatialEntityIdEXT id;
    uint32_t cap;               // the capability it is the context's by: an index into its caps
    uint32_t serial;            // a marker's; 0 for an anchor
    struct oxs_pose pose;       // an anchor's, in STAGE
    bool stopped;
};

struct context {
    struct object o;
    struct session *session;
    struct capconf caps[CAPS_MAX];
    uint32_t ncaps;
    struct known *known;
    size_t nknown, cap_known;
    uint32_t nanchors;
    uint32_t *announced;        // the markers a discovery has been recommended for, by serial
    size_t nannounced, cap_announced;
    struct context *next;       // in `contexts`
};

struct entity {
    struct object o;
    struct context *ctx;
    XrSpatialEntityIdEXT id;
    bool anchor;                // the handle xrCreateSpatialAnchorEXT made: the anchor lasts as long as it
};

struct snap_entity {
    XrSpatialEntityIdEXT id;
    XrSpatialEntityTrackingStateEXT state;
    unsigned components;        // C_*: those it has
    XrSpatialBounded2DDataEXT bounds;
    XrSpatialMarkerDataEXT data;
    XrPosef location;           // an anchor's
    char text[OXS_MARKER_TEXT]; // the buffer data.data names, for a QR code
};

struct snapshot {
    struct object o;
    struct context *ctx;
    unsigned components;        // C_*: those whose data it holds
    uint32_t n;
    struct snap_entity e[];
};

static pthread_mutex_t spatial_lock = PTHREAD_MUTEX_INITIALIZER;   // contexts' known entities
static uint64_t last_id, last_buffer;                               // under spatial_lock
static struct context *contexts;                                    // every live one, under spatial_lock

static XrPosef to_xr(const struct oxs_pose *p) { return (XrPosef){ { p->q[0], p->q[1], p->q[2], p->q[3] }, { p->p[0], p->p[1], p->p[2] } }; }

// ---- what is in the room, and what it is to a context ---------------------------------------------

// The room now; once the simulator has gone, as it was last read -- a marker is never taken to have left
// the room because the window was closed.
static struct oxs_room room_now(struct session *s)
{
    struct oxs_room r;
    pthread_mutex_lock(&s->track_lock);
    if (s->device_open && device()->room != NULL && device()->room(&r)) {
        s->room = r;
        s->room_read = true;
    } else if (s->room_read) {
        r = s->room;
    } else {
        oxs_room_home(&r);
    }
    pthread_mutex_unlock(&s->track_lock);
    return r;
}

static XrSpatialCapabilityEXT capability_of(const struct oxs_marker *m)
{
    switch (m->kind) {
        case OXS_MARKER_QR: return XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT;
        case OXS_MARKER_MICRO_QR: return XR_SPATIAL_CAPABILITY_MARKER_TRACKING_MICRO_QR_CODE_EXT;
        case OXS_MARKER_ARUCO: return XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT;
        case OXS_MARKER_APRIL_TAG: return XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT;
        default: return XR_SPATIAL_CAPABILITY_MAX_ENUM_EXT;
    }
}

// The capability a context tracks this marker by, or -1: its kind, and for ArUco and AprilTag its dictionary.
static int tracked_by(const struct context *c, const struct oxs_marker *m)
{
    const XrSpatialCapabilityEXT cap = capability_of(m);
    for (uint32_t i = 0; i < c->ncaps; i++)
        if (c->caps[i].cap == cap && (c->caps[i].dict == 0 || c->caps[i].dict == m->dict)) return (int)i;
    return -1;
}

static int anchor_cap(const struct context *c)
{
    for (uint32_t i = 0; i < c->ncaps; i++)
        if (c->caps[i].cap == XR_SPATIAL_CAPABILITY_ANCHOR_EXT) return (int)i;
    return -1;
}

static struct known *known_marker(struct context *c, uint32_t serial)
{
    for (size_t i = 0; i < c->nknown; i++)
        if (c->known[i].serial == serial && serial != 0) return &c->known[i];
    return NULL;
}

static struct known *known_by_id(struct context *c, XrSpatialEntityIdEXT id)
{
    for (size_t i = 0; i < c->nknown; i++)
        if (c->known[i].id == id) return &c->known[i];
    return NULL;
}

// A new entity of the context's, with an id never given before.
static struct known *add_known(struct context *c, uint32_t cap, uint32_t serial)
{
    if (c->nknown == c->cap_known) {
        const size_t want = c->cap_known ? c->cap_known * 2 : 16;
        struct known *g = realloc(c->known, want * sizeof *g);
        if (g == NULL) return NULL;
        c->known = g;
        c->cap_known = want;
    }
    struct known *k = &c->known[c->nknown++];
    *k = (struct known){ .id = (XrSpatialEntityIdEXT)++last_id, .cap = cap, .serial = serial };
    return k;
}

// One marker's data as a snapshot keeps it, in `base`.
static void fill_marker(struct snap_entity *e, const struct capconf *cc, const struct oxs_marker *m, const struct oxs_pose *base,
                        unsigned want)
{
    if (want & cc->components & C_BOUNDED) {
        const struct oxs_pose p = oxs_pose_relative(base, &m->pose);
        const float side = cc->fixed_size > 0 ? cc->fixed_size : m->size;
        e->bounds = (XrSpatialBounded2DDataEXT){ to_xr(&p), { side, side } };
    }
    if (want & cc->components & C_MARKER) {
        const bool qr = m->kind == OXS_MARKER_QR || m->kind == OXS_MARKER_MICRO_QR;
        e->data.capability = cc->cap;
        e->data.markerId = qr ? 0 : m->id;
        e->data.data = (XrSpatialBufferEXT){ XR_NULL_SPATIAL_BUFFER_ID_EXT, XR_SPATIAL_BUFFER_TYPE_UNKNOWN_EXT };
        if (qr) {
            snprintf(e->text, sizeof e->text, "%s", m->text);
            e->data.data = (XrSpatialBufferEXT){ (XrSpatialBufferIdEXT)++last_buffer, XR_SPATIAL_BUFFER_TYPE_STRING_EXT };
        }
    }
}

static void fill_anchor(struct snap_entity *e, const struct known *k, const struct oxs_pose *base, unsigned want)
{
    if (want & C_ANCHOR) {
        const struct oxs_pose p = oxs_pose_relative(base, &k->pose);
        e->location = to_xr(&p);
    }
}

// ---- events ------------------------------------------------------------------------------------------
//
// XrEventDataSpatialDiscoveryRecommendedEXT, once for each marker a context tracks that comes into view
// before it has discovered it -- once only, so an application whose discoveries leave it out (a filter of
// components or states) is not told again at every poll. Not before the context was completed (it does not
// exist till then), and none left in the queue once it is destroyed. Anchors are the application's own: it
// knows when it has made one.

static bool announced(const struct context *c, uint32_t serial)
{
    for (size_t i = 0; i < c->nannounced; i++)
        if (c->announced[i] == serial) return true;
    return false;
}

void spatial_poll(struct session *s)
{
    if (s == NULL) return;
    const struct oxs_room room = room_now(s);
    const struct oxs_pose head = session_tracking(s).head;
    pthread_mutex_lock(&spatial_lock);
    for (struct context *c = contexts; c != NULL; c = c->next) {
        if (c->session != s) continue;
        bool post = false;
        for (uint32_t k = 0; k < room.count && k < OXS_MARKERS_MAX; k++) {
            const struct oxs_marker *m = &room.marker[k];
            if (tracked_by(c, m) < 0 || known_marker(c, m->serial) != NULL || announced(c, m->serial) || !oxs_marker_seen(&head, m))
                continue;
            if (c->nannounced == c->cap_announced) {
                const size_t want = c->cap_announced ? c->cap_announced * 2 : 16;
                uint32_t *g = realloc(c->announced, want * sizeof *g);
                if (g == NULL) break;
                c->announced = g;
                c->cap_announced = want;
            }
            c->announced[c->nannounced++] = m->serial;
            post = true;
        }
        if (post) {
            const XrEventDataSpatialDiscoveryRecommendedEXT e = {
                .type = XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT,
                .spatialContext = (XrSpatialContextEXT)(uintptr_t)c,
            };
            post_event(s->in, &e, sizeof e);
        }
    }
    pthread_mutex_unlock(&spatial_lock);
}

// ---- capabilities ------------------------------------------------------------------------------------

static const struct {
    XrSpatialCapabilityEXT cap;
    XrStructureType config;     // its configuration's structure
    unsigned components;        // C_*: what its entities have
    bool marker;                // XR_EXT_spatial_marker_tracking's, else XR_EXT_spatial_anchor's
} CAPABILITIES[] = {
    { XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT, C_MARKER | C_BOUNDED, true },
    { XR_SPATIAL_CAPABILITY_MARKER_TRACKING_MICRO_QR_CODE_EXT, XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_MICRO_QR_CODE_EXT, C_MARKER | C_BOUNDED, true },
    { XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT, XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ARUCO_MARKER_EXT, C_MARKER | C_BOUNDED, true },
    { XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT, XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_APRIL_TAG_EXT, C_MARKER | C_BOUNDED, true },
    { XR_SPATIAL_CAPABILITY_ANCHOR_EXT, XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ANCHOR_EXT, C_ANCHOR, false },
};
#define NCAPABILITIES (sizeof CAPABILITIES / sizeof CAPABILITIES[0])

// A capability is offered only with its extension enabled: neither enabled, there is none at all.
static int capability_index(struct instance *in, XrSpatialCapabilityEXT cap)
{
    for (size_t i = 0; i < NCAPABILITIES; i++)
        if (CAPABILITIES[i].cap == cap && (CAPABILITIES[i].marker ? in->marker_tracking : in->spatial_anchor)) return (int)i;
    return -1;
}

#define SYSTEM_CHECK(instance, id)                                                                    \
    struct instance *in = (struct instance *)find(H(instance), MAGIC_INSTANCE);                       \
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;                                                   \
    if (!system_ok(in, (id))) return XR_ERROR_SYSTEM_INVALID

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateSpatialCapabilitiesEXT(XrInstance instance, XrSystemId id, uint32_t cap,
                                                                         uint32_t *count, XrSpatialCapabilityEXT *out)
{
    SYSTEM_CHECK(instance, id);
    XrSpatialCapabilityEXT offered[NCAPABILITIES];
    uint32_t n = 0;
    for (size_t i = 0; i < NCAPABILITIES; i++)
        if (capability_index(in, CAPABILITIES[i].cap) >= 0) offered[n++] = CAPABILITIES[i].cap;
    TWO_CALL(cap, count, out, offered, n);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateSpatialCapabilityComponentTypesEXT(XrInstance instance, XrSystemId id,
    XrSpatialCapabilityEXT capability, XrSpatialCapabilityComponentTypesEXT *out)
{
    SYSTEM_CHECK(instance, id);
    if (out == NULL || out->type != XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT) return XR_ERROR_VALIDATION_FAILURE;
    const int i = capability_index(in, capability);
    if (i < 0) return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
    static const XrSpatialComponentTypeEXT marker[] = { XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT, XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT };
    static const XrSpatialComponentTypeEXT anchor[] = { XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT };
    if (CAPABILITIES[i].marker) TWO_CALL(out->componentTypeCapacityInput, &out->componentTypeCountOutput, out->componentTypes, marker, 2);
    else TWO_CALL(out->componentTypeCapacityInput, &out->componentTypeCountOutput, out->componentTypes, anchor, 1);
    return XR_SUCCESS;
}

// Both of marker tracking's: a size the application gives every marker, and markers that do not move. None
// of the anchor's.
static const XrSpatialCapabilityFeatureEXT MARKER_FEATURES[] = {
    XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_FIXED_SIZE_MARKERS_EXT,
    XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_STATIC_MARKERS_EXT,
};

static XRAPI_ATTR XrResult XRAPI_CALL oxs_EnumerateSpatialCapabilityFeaturesEXT(XrInstance instance, XrSystemId id,
    XrSpatialCapabilityEXT capability, uint32_t cap, uint32_t *count, XrSpatialCapabilityFeatureEXT *out)
{
    SYSTEM_CHECK(instance, id);
    const int i = capability_index(in, capability);
    if (i < 0) return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
    TWO_CALL(cap, count, out, MARKER_FEATURES, CAPABILITIES[i].marker ? 2 : 0);
    return XR_SUCCESS;
}

// ---- futures -------------------------------------------------------------------------------------------

static struct future *make_future(int kind, struct object *owner, struct instance *in)
{
    struct future *f = make(sizeof *f, MAGIC_FUTURE, &in->o, owner);
    if (f == NULL) return NULL;
    f->kind = kind;
    f->owner = owner;
    return f;
}

// A future of `kind` made from `owner`: XR_ERROR_FUTURE_INVALID_EXT for one that is not (any more), or was
// made for another function; XR_ERROR_HANDLE_INVALID for one made from another handle.
static XrResult future_of(XrFutureEXT future, int kind, struct object *owner, struct future **out)
{
    struct future *f = (struct future *)find(H(future), MAGIC_FUTURE);
    if (f == NULL || f->kind != kind) return XR_ERROR_FUTURE_INVALID_EXT;
    if (f->owner != owner) return XR_ERROR_HANDLE_INVALID;
    *out = f;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_PollFutureEXT(XrInstance instance, const XrFuturePollInfoEXT *info, XrFuturePollResultEXT *result)
{
    struct object *in = find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || result == NULL || info->type != XR_TYPE_FUTURE_POLL_INFO_EXT || result->type != XR_TYPE_FUTURE_POLL_RESULT_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
    struct object *f = find(H(info->future), MAGIC_FUTURE);
    if (f == NULL || f->instance != in) return XR_ERROR_FUTURE_INVALID_EXT;
    result->state = XR_FUTURE_STATE_READY_EXT;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CancelFutureEXT(XrInstance instance, const XrFutureCancelInfoEXT *info)
{
    struct object *in = find(H(instance), MAGIC_INSTANCE);
    if (in == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || info->type != XR_TYPE_FUTURE_CANCEL_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    struct object *f = find(H(info->future), MAGIC_FUTURE);
    if (f == NULL || f->instance != in) return XR_ERROR_FUTURE_INVALID_EXT;
    destroy(f);
    return XR_SUCCESS;
}

// ---- contexts ------------------------------------------------------------------------------------------

// One capability's configuration, checked: the error OpenXR gives for each way it can be wrong.
static XrResult configure(struct instance *in, const XrSpatialCapabilityConfigurationBaseHeaderEXT *b, struct capconf *out)
{
    if (b == NULL) return XR_ERROR_VALIDATION_FAILURE;
    const int i = capability_index(in, b->capability);
    if (i < 0) return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
    if (b->type != CAPABILITIES[i].config) return XR_ERROR_VALIDATION_FAILURE;   // a configuration of another capability
    if (b->enabledComponentCount == 0) return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
    if (b->enabledComponents == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *out = (struct capconf){ .cap = b->capability };
    for (uint32_t k = 0; k < b->enabledComponentCount; k++) {
        const unsigned bit = component_bit(b->enabledComponents[k]);
        if ((bit & CAPABILITIES[i].components) == 0) return XR_ERROR_SPATIAL_COMPONENT_UNSUPPORTED_FOR_CAPABILITY_EXT;
        out->components |= bit;
    }
    if (b->type == XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ARUCO_MARKER_EXT) {
        out->dict = (int)((const XrSpatialCapabilityConfigurationArucoMarkerEXT *)b)->arUcoDict;
        if (oxs_marker_dict_size(OXS_MARKER_ARUCO, out->dict) == 0) return XR_ERROR_VALIDATION_FAILURE;
    }
    if (b->type == XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_APRIL_TAG_EXT) {
        out->dict = (int)((const XrSpatialCapabilityConfigurationAprilTagEXT *)b)->aprilDict;
        if (oxs_marker_dict_size(OXS_MARKER_APRIL_TAG, out->dict) == 0) return XR_ERROR_VALIDATION_FAILURE;
    }
    // Marker tracking's features, both enumerated: a size the application gives every marker; markers it says
    // are static, which every simulated one is anyway. On another capability they are not its, and ignored.
    if (CAPABILITIES[i].marker)
        for (const XrBaseInStructure *n = b->next; n != NULL; n = n->next)
            if (n->type == XR_TYPE_SPATIAL_MARKER_SIZE_EXT) {
                const float side = ((const XrSpatialMarkerSizeEXT *)n)->markerSideLength;
                if (!(side > 0)) return XR_ERROR_VALIDATION_FAILURE;
                out->fixed_size = side;
            }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialContextAsyncEXT(XrSession session, const XrSpatialContextCreateInfoEXT *info,
                                                                      XrFutureEXT *future)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || future == NULL || info->type != XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    if (info->capabilityConfigCount == 0) return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
    if (info->capabilityConfigs == NULL) return XR_ERROR_VALIDATION_FAILURE;
    struct capconf caps[CAPS_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; i < info->capabilityConfigCount; i++) {
        struct capconf c;
        const XrResult r = configure(s->in, info->capabilityConfigs[i], &c);
        if (r != XR_SUCCESS) return r;
        for (uint32_t k = 0; k < n; k++)
            if (caps[k].cap == c.cap) return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;   // twice
        caps[n++] = c;   // at most CAPS_MAX: no capability twice, and there are that many
    }
    struct future *f = make_future(FUTURE_CONTEXT, &s->o, s->in);
    if (f == NULL) return XR_ERROR_OUT_OF_MEMORY;
    memcpy(f->caps, caps, sizeof caps);
    f->ncaps = n;
    *future = (XrFutureEXT)(uintptr_t)f;
    return XR_SUCCESS;
}

static void context_fini(struct object *o)
{
    struct context *c = (struct context *)o;
    pthread_mutex_lock(&spatial_lock);
    for (struct context **p = &contexts; *p != NULL; p = &(*p)->next)
        if (*p == c) { *p = c->next; break; }
    // No recommendation left for the application to find after this.
    unpost_events(c->session->in, XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT, H(c));
    pthread_mutex_unlock(&spatial_lock);
    free(c->known);
    free(c->announced);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialContextCompleteEXT(XrSession session, XrFutureEXT future,
                                                                         XrCreateSpatialContextCompletionEXT *out)
{
    struct session *s = (struct session *)find(H(session), MAGIC_SESSION);
    if (s == NULL) return XR_ERROR_HANDLE_INVALID;
    if (out == NULL || out->type != XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT) return XR_ERROR_VALIDATION_FAILURE;
    struct future *f;
    const XrResult r = future_of(future, FUTURE_CONTEXT, &s->o, &f);
    if (r != XR_SUCCESS) return r;
    struct context *c = make(sizeof *c, MAGIC_SPATIAL_CONTEXT, &s->in->o, &s->o);
    if (c == NULL) {
        out->futureResult = XR_ERROR_OUT_OF_MEMORY;
        out->spatialContext = XR_NULL_HANDLE;
    } else {
        c->o.fini = context_fini;
        c->session = s;
        memcpy(c->caps, f->caps, sizeof c->caps);
        c->ncaps = f->ncaps;
        pthread_mutex_lock(&spatial_lock);
        c->next = contexts;
        contexts = c;
        pthread_mutex_unlock(&spatial_lock);
        out->futureResult = XR_SUCCESS;
        out->spatialContext = (XrSpatialContextEXT)(uintptr_t)c;
    }
    destroy(&f->o);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroySpatialContextEXT(XrSpatialContextEXT context)
{
    struct object *o = find(H(context), MAGIC_SPATIAL_CONTEXT);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

// ---- snapshots -----------------------------------------------------------------------------------------

// Which components a snapshot asks for: those listed, each enabled in the context; none listed is all of them.
static XrResult wanted(struct context *c, const XrSpatialComponentTypeEXT *list, uint32_t n, unsigned *want)
{
    unsigned enabled = 0;
    for (uint32_t i = 0; i < c->ncaps; i++) enabled |= c->caps[i].components;
    if (n == 0) { *want = enabled; return XR_SUCCESS; }
    if (list == NULL) return XR_ERROR_VALIDATION_FAILURE;
    *want = 0;
    for (uint32_t i = 0; i < n; i++) {
        const unsigned bit = component_bit(list[i]);
        if ((bit & enabled) == 0) return XR_ERROR_SPATIAL_COMPONENT_NOT_ENABLED_EXT;
        *want |= bit;
    }
    return XR_SUCCESS;
}

static XrSpatialEntityTrackingStateEXT state_filter(const void *next)
{
    for (const XrBaseInStructure *n = next; n != NULL; n = n->next)
        if (n->type == XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT) return ((const XrSpatialFilterTrackingStateEXT *)n)->trackingState;
    return 0;
}

static struct snapshot *make_snapshot(struct context *c, size_t n, unsigned components)
{
    struct snapshot *sn = make(sizeof *sn + n * sizeof sn->e[0], MAGIC_SPATIAL_SNAPSHOT, &c->session->in->o, &c->o);
    if (sn == NULL) return NULL;
    sn->ctx = c;
    sn->components = components;
    return sn;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialDiscoverySnapshotAsyncEXT(XrSpatialContextEXT context,
    const XrSpatialDiscoverySnapshotCreateInfoEXT *info, XrFutureEXT *future)
{
    struct context *c = (struct context *)find(H(context), MAGIC_SPATIAL_CONTEXT);
    if (c == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || future == NULL || info->type != XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    unsigned want;
    const XrResult r = wanted(c, info->componentTypes, info->componentTypeCount, &want);
    if (r != XR_SUCCESS) return r;
    struct future *f = make_future(FUTURE_DISCOVERY, &c->o, c->session->in);
    if (f == NULL) return XR_ERROR_OUT_OF_MEMORY;
    f->want = want;
    f->filter = state_filter(info->next);
    *future = (XrFutureEXT)(uintptr_t)f;
    return XR_SUCCESS;
}

// DISCOVERY: every marker the context tracks that the headset sees now, and every anchor of the context's
// still there, as they are in `baseSpace` at `time` -- those with at least one of the components asked for.
// Each is TRACKING (what is out of view is not discovered), so a filter for any other state finds none, and
// a base space that cannot be located finds none either.
static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialDiscoverySnapshotCompleteEXT(XrSpatialContextEXT context,
    const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT *info, XrCreateSpatialDiscoverySnapshotCompletionEXT *out)
{
    struct context *c = (struct context *)find(H(context), MAGIC_SPATIAL_CONTEXT);
    if (c == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT ||
        out->type != XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
    struct space *base = (struct space *)find(H(info->baseSpace), MAGIC_SPACE);
    if (base == NULL || base->session != c->session) return XR_ERROR_HANDLE_INVALID;
    if (info->time <= 0) return XR_ERROR_TIME_INVALID;
    struct future *f;
    const XrResult r = future_of(info->future, FUTURE_DISCOVERY, &c->o, &f);
    if (r != XR_SUCCESS) return r;

    const struct oxs_room room = room_now(c->session);
    const struct oxs_pose head = session_tracking(c->session).head;
    struct oxs_pose b;
    const bool any = space_in_stage(base, info->time, &b) &&
                     (f->filter == 0 || f->filter == XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT);
    const unsigned want = f->want;
    destroy(&f->o);
    pthread_mutex_lock(&spatial_lock);
    struct snapshot *sn = make_snapshot(c, OXS_MARKERS_MAX + c->nanchors, want);
    if (sn == NULL) {
        pthread_mutex_unlock(&spatial_lock);
        out->futureResult = XR_ERROR_OUT_OF_MEMORY;
        out->snapshot = XR_NULL_HANDLE;
        return XR_SUCCESS;
    }
    for (uint32_t i = 0; any && i < room.count && i < OXS_MARKERS_MAX; i++) {
        const struct oxs_marker *m = &room.marker[i];
        const int cap = tracked_by(c, m);
        if (cap < 0 || (c->caps[cap].components & want) == 0 || !oxs_marker_seen(&head, m)) continue;
        const struct known *k = known_marker(c, m->serial);
        if (k == NULL && (k = add_known(c, (uint32_t)cap, m->serial)) == NULL) break;
        struct snap_entity *e = &sn->e[sn->n++];
        e->id = k->id;
        e->state = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
        e->components = c->caps[cap].components;
        fill_marker(e, &c->caps[cap], m, &b, want);
    }
    for (size_t i = 0; any && i < c->nknown; i++) {
        const struct known *k = &c->known[i];
        if (k->serial != 0 || k->stopped || (c->caps[k->cap].components & want) == 0) continue;
        struct snap_entity *e = &sn->e[sn->n++];
        e->id = k->id;
        e->state = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
        e->components = c->caps[k->cap].components;
        fill_anchor(e, k, &b, want);
    }
    pthread_mutex_unlock(&spatial_lock);
    out->futureResult = XR_SUCCESS;
    out->snapshot = (XrSpatialSnapshotEXT)(uintptr_t)sn;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroySpatialSnapshotEXT(XrSpatialSnapshotEXT snapshot)
{
    struct object *o = find(H(snapshot), MAGIC_SPATIAL_SNAPSHOT);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

// ---- queries -------------------------------------------------------------------------------------------
//
// The entities with every component the condition names (all of them, with none named) and the tracking
// state it filters for; the data of each component a list is chained for, at each entity's index -- an
// entity that is not TRACKING has its index left as the application had it.

static XRAPI_ATTR XrResult XRAPI_CALL oxs_QuerySpatialComponentDataEXT(XrSpatialSnapshotEXT snapshot,
    const XrSpatialComponentDataQueryConditionEXT *cond, XrSpatialComponentDataQueryResultEXT *out)
{
    struct snapshot *sn = (struct snapshot *)find(H(snapshot), MAGIC_SPATIAL_SNAPSHOT);
    if (sn == NULL) return XR_ERROR_HANDLE_INVALID;
    if (cond == NULL || out == NULL || cond->type != XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT ||
        out->type != XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
    if (cond->componentTypeCount > 0 && cond->componentTypes == NULL) return XR_ERROR_VALIDATION_FAILURE;
    // A component no entity here can have matches none.
    unsigned need = 0;
    bool unknown = false;
    for (uint32_t i = 0; i < cond->componentTypeCount; i++) {
        const unsigned bit = component_bit(cond->componentTypes[i]);
        need |= bit;
        unknown |= bit == 0;
    }
    const XrSpatialEntityTrackingStateEXT filter = state_filter(cond->next);

    XrSpatialComponentBounded2DListEXT *bounds = NULL;
    XrSpatialComponentMarkerListEXT *markers = NULL;
    XrSpatialComponentAnchorListEXT *anchors = NULL;
    unsigned lists = 0;
    for (XrBaseOutStructure *n = out->next; n != NULL; n = n->next) {
        if (n->type == XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT) { bounds = (XrSpatialComponentBounded2DListEXT *)n; lists |= C_BOUNDED; }
        else if (n->type == XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT) { markers = (XrSpatialComponentMarkerListEXT *)n; lists |= C_MARKER; }
        else if (n->type == XR_TYPE_SPATIAL_COMPONENT_ANCHOR_LIST_EXT) { anchors = (XrSpatialComponentAnchorListEXT *)n; lists |= C_ANCHOR; }
        else if (n->type == XR_TYPE_SPATIAL_COMPONENT_BOUNDED_3D_LIST_EXT || n->type == XR_TYPE_SPATIAL_COMPONENT_PARENT_LIST_EXT ||
                 n->type == XR_TYPE_SPATIAL_COMPONENT_MESH_3D_LIST_EXT)
            return XR_ERROR_VALIDATION_FAILURE;   // components nothing here has, so none the condition can name
    }
    // A list for a component the condition does not name, or whose data the snapshot does not hold.
    if ((cond->componentTypeCount > 0 && (lists & ~need) != 0) || (lists & ~sn->components) != 0) return XR_ERROR_VALIDATION_FAILURE;

    uint32_t *match = sn->n > 0 ? malloc(sn->n * sizeof *match) : NULL, n = 0;
    if (sn->n > 0 && match == NULL) return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < sn->n && !unknown; i++) {
        const struct snap_entity *e = &sn->e[i];
        if ((e->components & need) != need) continue;
        if (filter != 0 && e->state != filter) continue;
        match[n++] = i;
    }
    out->entityIdCountOutput = out->entityStateCountOutput = n;
    XrResult r = XR_SUCCESS;
    if (out->entityIdCapacityInput == 0 && out->entityStateCapacityInput == 0) goto done;   // the first of the two calls
    r = XR_ERROR_SIZE_INSUFFICIENT;
    if ((out->entityIdCapacityInput != 0 && out->entityIdCapacityInput < n) ||
        (out->entityStateCapacityInput != 0 && out->entityStateCapacityInput < n) ||
        (bounds != NULL && bounds->boundCount < n) || (markers != NULL && markers->markerCount < n) ||
        (anchors != NULL && anchors->locationCount < n))
        goto done;
    r = XR_ERROR_VALIDATION_FAILURE;
    if ((out->entityIdCapacityInput != 0 && out->entityIds == NULL) || (out->entityStateCapacityInput != 0 && out->entityStates == NULL) ||
        (n > 0 && ((bounds != NULL && bounds->bounds == NULL) || (markers != NULL && markers->markers == NULL) ||
                   (anchors != NULL && anchors->locations == NULL))))
        goto done;
    r = XR_SUCCESS;
    for (uint32_t k = 0; k < n; k++) {
        const struct snap_entity *e = &sn->e[match[k]];
        if (out->entityIdCapacityInput != 0) out->entityIds[k] = e->id;
        if (out->entityStateCapacityInput != 0) out->entityStates[k] = e->state;
        if (e->state != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT) continue;
        if (bounds != NULL && (e->components & C_BOUNDED)) bounds->bounds[k] = e->bounds;
        if (markers != NULL && (e->components & C_MARKER)) markers->markers[k] = e->data;
        if (anchors != NULL && (e->components & C_ANCHOR)) anchors->locations[k] = e->location;
    }
done:
    free(match);
    return r;
}

// ---- entities and update snapshots ---------------------------------------------------------------------

static struct entity *make_entity(struct context *c, XrSpatialEntityIdEXT id, bool anchor);

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialEntityFromIdEXT(XrSpatialContextEXT context,
    const XrSpatialEntityFromIdCreateInfoEXT *info, XrSpatialEntityEXT *out)
{
    struct context *c = (struct context *)find(H(context), MAGIC_SPATIAL_CONTEXT);
    if (c == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    pthread_mutex_lock(&spatial_lock);
    const bool ok = info->entityId != XR_NULL_SPATIAL_ENTITY_ID_EXT && known_by_id(c, info->entityId) != NULL;
    pthread_mutex_unlock(&spatial_lock);
    if (!ok) return XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT;
    struct entity *e = make_entity(c, info->entityId, false);
    if (e == NULL) return XR_ERROR_OUT_OF_MEMORY;
    *out = (XrSpatialEntityEXT)(uintptr_t)e;
    return XR_SUCCESS;
}

// The handle an anchor was made with: destroyed, the anchor goes, and is STOPPED for any other handle to it.
static void entity_fini(struct object *o)
{
    struct entity *e = (struct entity *)o;
    if (!e->anchor) return;
    pthread_mutex_lock(&spatial_lock);
    struct known *k = known_by_id(e->ctx, e->id);
    if (k != NULL && !k->stopped) {
        k->stopped = true;
        e->ctx->nanchors--;
    }
    pthread_mutex_unlock(&spatial_lock);
}

static struct entity *make_entity(struct context *c, XrSpatialEntityIdEXT id, bool anchor)
{
    struct entity *e = make(sizeof *e, MAGIC_SPATIAL_ENTITY, &c->session->in->o, &c->o);
    if (e == NULL) return NULL;
    e->o.fini = entity_fini;
    e->ctx = c;
    e->id = id;
    e->anchor = anchor;
    return e;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxs_DestroySpatialEntityEXT(XrSpatialEntityEXT entity)
{
    struct object *o = find(H(entity), MAGIC_SPATIAL_ENTITY);
    if (o == NULL) return XR_ERROR_HANDLE_INVALID;
    destroy(o);
    return XR_SUCCESS;
}

// UPDATE: the entities named, each as it is now, with the data of the TRACKING ones in `baseSpace` at `time`:
// a marker TRACKING in view, PAUSED out of it, STOPPED (for good) once it has left the room; an anchor
// TRACKING, or PAUSED while the base space cannot be located, STOPPED once it has gone.
static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialUpdateSnapshotEXT(XrSpatialContextEXT context,
    const XrSpatialUpdateSnapshotCreateInfoEXT *info, XrSpatialSnapshotEXT *out)
{
    struct context *c = (struct context *)find(H(context), MAGIC_SPATIAL_CONTEXT);
    if (c == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || out == NULL || info->type != XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    if (info->entityCount == 0 || info->entities == NULL) return XR_ERROR_VALIDATION_FAILURE;
    struct space *base = (struct space *)find(H(info->baseSpace), MAGIC_SPACE);
    if (base == NULL || base->session != c->session) return XR_ERROR_HANDLE_INVALID;
    if (info->time <= 0) return XR_ERROR_TIME_INVALID;
    unsigned want;
    const XrResult r = wanted(c, info->componentTypes, info->componentTypeCount, &want);
    if (r != XR_SUCCESS) return r;
    for (uint32_t i = 0; i < info->entityCount; i++) {
        struct entity *e = (struct entity *)find(H(info->entities[i]), MAGIC_SPATIAL_ENTITY);
        if (e == NULL || e->ctx != c) return XR_ERROR_HANDLE_INVALID;
    }

    const struct oxs_room room = room_now(c->session);
    const struct oxs_pose head = session_tracking(c->session).head;
    struct oxs_pose b;
    const bool located = space_in_stage(base, info->time, &b);
    struct snapshot *sn = make_snapshot(c, info->entityCount, want);
    if (sn == NULL) return XR_ERROR_OUT_OF_MEMORY;
    pthread_mutex_lock(&spatial_lock);
    for (uint32_t i = 0; i < info->entityCount; i++) {
        const struct entity *ent = (const struct entity *)(uintptr_t)H(info->entities[i]);
        struct known *k = known_by_id(c, ent->id);
        if (k == NULL) continue;
        const struct oxs_marker *m = NULL;
        if (k->serial != 0) {
            for (uint32_t j = 0; j < room.count && j < OXS_MARKERS_MAX && !k->stopped; j++)
                if (room.marker[j].serial == k->serial) m = &room.marker[j];
            if (m == NULL) k->stopped = true;
        }
        // Its components are its capability's, also once it has stopped; it is in the snapshot if it has at
        // least one of those asked for.
        const struct capconf *cc = &c->caps[k->cap];
        if ((cc->components & want) == 0) continue;
        struct snap_entity *e = &sn->e[sn->n++];
        e->id = k->id;
        e->components = cc->components;
        if (k->stopped) e->state = XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT;
        else if (!located || (m != NULL && !oxs_marker_seen(&head, m))) e->state = XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT;
        else {
            e->state = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
            if (m != NULL) fill_marker(e, cc, m, &b, want);
            else fill_anchor(e, k, &b, want);
        }
    }
    pthread_mutex_unlock(&spatial_lock);
    *out = (XrSpatialSnapshotEXT)(uintptr_t)sn;
    return XR_SUCCESS;
}

// ---- anchors -------------------------------------------------------------------------------------------
//
// XR_EXT_spatial_anchor: a pose in a base space at a time, kept in STAGE. TRACKING from the start: nothing
// here has to map the room first.

static XRAPI_ATTR XrResult XRAPI_CALL oxs_CreateSpatialAnchorEXT(XrSpatialContextEXT context, const XrSpatialAnchorCreateInfoEXT *info,
                                                                XrSpatialEntityIdEXT *id, XrSpatialEntityEXT *out)
{
    struct context *c = (struct context *)find(H(context), MAGIC_SPATIAL_CONTEXT);
    if (c == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || id == NULL || out == NULL || info->type != XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    const int cap = anchor_cap(c);
    if (cap < 0) return XR_ERROR_VALIDATION_FAILURE;   // the context was not configured for anchors
    struct space *base = (struct space *)find(H(info->baseSpace), MAGIC_SPACE);
    if (base == NULL || base->session != c->session) return XR_ERROR_HANDLE_INVALID;
    if (info->time <= 0) return XR_ERROR_TIME_INVALID;
    const float q[4] = { info->pose.orientation.x, info->pose.orientation.y, info->pose.orientation.z, info->pose.orientation.w };
    if (!oxs_quat_valid(q)) return XR_ERROR_POSE_INVALID;
    struct oxs_pose b;
    if (!space_in_stage(base, info->time, &b)) {
        say("xrCreateSpatialAnchorEXT: the base space cannot be located, so there is nowhere to put the anchor");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    struct oxs_pose p = { { q[0], q[1], q[2], q[3] }, { info->pose.position.x, info->pose.position.y, info->pose.position.z } };
    oxs_quat_normalize(p.q);
    pthread_mutex_lock(&spatial_lock);
    struct known *k = c->nanchors < ANCHORS_MAX ? add_known(c, (uint32_t)cap, 0) : NULL;
    if (k != NULL) {
        k->pose = oxs_pose_compose(&b, &p);
        c->nanchors++;
    }
    const XrSpatialEntityIdEXT made = k != NULL ? k->id : XR_NULL_SPATIAL_ENTITY_ID_EXT;
    const bool full = c->nanchors >= ANCHORS_MAX && k == NULL;
    pthread_mutex_unlock(&spatial_lock);
    if (k == NULL) return full ? XR_ERROR_LIMIT_REACHED : XR_ERROR_OUT_OF_MEMORY;
    struct entity *e = make_entity(c, made, true);
    if (e == NULL) {
        pthread_mutex_lock(&spatial_lock);
        struct known *gone = known_by_id(c, made);
        if (gone != NULL) { gone->stopped = true; c->nanchors--; }
        pthread_mutex_unlock(&spatial_lock);
        return XR_ERROR_OUT_OF_MEMORY;
    }
    *id = made;
    *out = (XrSpatialEntityEXT)(uintptr_t)e;
    return XR_SUCCESS;
}

// ---- buffers -------------------------------------------------------------------------------------------
//
// The only buffers are QR codes' text, as strings; the count is the characters with the NUL that ends them,
// as OpenXR's other strings are counted.

static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetSpatialBufferStringEXT(XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT *info,
                                                                   uint32_t cap, uint32_t *count, char *buffer)
{
    struct snapshot *sn = (struct snapshot *)find(H(snapshot), MAGIC_SPATIAL_SNAPSHOT);
    if (sn == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || count == NULL || info->type != XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    for (uint32_t i = 0; i < sn->n; i++) {
        const struct snap_entity *e = &sn->e[i];
        if (e->state != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT || e->data.data.bufferId != info->bufferId ||
            info->bufferId == XR_NULL_SPATIAL_BUFFER_ID_EXT)
            continue;
        const uint32_t n = (uint32_t)strlen(e->text) + 1;
        *count = n;
        if (cap == 0) return XR_SUCCESS;
        if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
        if (buffer == NULL) return XR_ERROR_VALIDATION_FAILURE;
        memcpy(buffer, e->text, n);
        return XR_SUCCESS;
    }
    return XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT;
}

// Every other type: a buffer of this snapshot is a string, and asking for it as anything else is the
// wrong type; any other id is not one of its buffers.
static XrResult other_buffer(XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT *info, uint32_t *count)
{
    struct snapshot *sn = (struct snapshot *)find(H(snapshot), MAGIC_SPATIAL_SNAPSHOT);
    if (sn == NULL) return XR_ERROR_HANDLE_INVALID;
    if (info == NULL || count == NULL || info->type != XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT) return XR_ERROR_VALIDATION_FAILURE;
    for (uint32_t i = 0; i < sn->n; i++)
        if (sn->e[i].state == XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT && info->bufferId != XR_NULL_SPATIAL_BUFFER_ID_EXT &&
            sn->e[i].data.data.bufferId == info->bufferId)
            return XR_ERROR_VALIDATION_FAILURE;
    return XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT;
}

#define OTHER_BUFFER(Name, T)                                                                                  \
    static XRAPI_ATTR XrResult XRAPI_CALL oxs_GetSpatialBuffer##Name##EXT(XrSpatialSnapshotEXT snapshot,      \
        const XrSpatialBufferGetInfoEXT *info, uint32_t cap, uint32_t *count, T *buffer)                       \
    {                                                                                                          \
        (void)cap; (void)buffer;                                                                               \
        return other_buffer(snapshot, info, count);                                                            \
    }
OTHER_BUFFER(Uint8, uint8_t)
OTHER_BUFFER(Uint16, uint16_t)
OTHER_BUFFER(Uint32, uint32_t)
OTHER_BUFFER(Float, float)
OTHER_BUFFER(Vector2f, XrVector2f)
OTHER_BUFFER(Vector3f, XrVector3f)

const struct entry future_functions[] = {
    ENTRY(PollFutureEXT) ENTRY(CancelFutureEXT)
    { NULL, NULL }
};

const struct entry spatial_functions[] = {
    ENTRY(EnumerateSpatialCapabilitiesEXT) ENTRY(EnumerateSpatialCapabilityComponentTypesEXT)
    ENTRY(EnumerateSpatialCapabilityFeaturesEXT) ENTRY(CreateSpatialContextAsyncEXT) ENTRY(CreateSpatialContextCompleteEXT)
    ENTRY(DestroySpatialContextEXT) ENTRY(CreateSpatialDiscoverySnapshotAsyncEXT) ENTRY(CreateSpatialDiscoverySnapshotCompleteEXT)
    ENTRY(QuerySpatialComponentDataEXT) ENTRY(DestroySpatialSnapshotEXT) ENTRY(CreateSpatialEntityFromIdEXT)
    ENTRY(DestroySpatialEntityEXT) ENTRY(CreateSpatialUpdateSnapshotEXT) ENTRY(GetSpatialBufferStringEXT)
    ENTRY(GetSpatialBufferUint8EXT) ENTRY(GetSpatialBufferUint16EXT) ENTRY(GetSpatialBufferUint32EXT)
    ENTRY(GetSpatialBufferFloatEXT) ENTRY(GetSpatialBufferVector2fEXT) ENTRY(GetSpatialBufferVector3fEXT)
    { NULL, NULL }
};

const struct entry anchor_functions[] = {
    ENTRY(CreateSpatialAnchorEXT)
    { NULL, NULL }
};
