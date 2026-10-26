#include "alt111.h"
#include <string.h>

int alt111_eta_disabled_url(const char *saved, char *out, size_t cap)
{
    const char *p;
    size_t prefix, tail;
    if (!saved || !out || cap == 0u) return ALT111_INVALID;
    p = saved;
    while ((p = strstr(p, "showETA=yes")) != NULL) {
        if ((p == saved || p[-1] == '?' || p[-1] == '&') &&
            (p[11] == '\0' || p[11] == '&')) break;
        p += 11;
    }
    if (!p) return ALT111_INVALID;
    prefix = (size_t)(p - saved) + 8u; /* include "showETA=" */
    tail = strlen(p + 11);
    if (prefix + 2u + tail + 1u > cap) return ALT111_NOMEM;
    memcpy(out, saved, prefix);
    memcpy(out + prefix, "no", 2u);
    memcpy(out + prefix + 2u, p + 11, tail + 1u);
    return ALT111_OK;
}

static uint64_t after(uint64_t now, unsigned delay)
{
    return UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
}

static void cancel(struct alt111_control *c)
{
    memset(&c->pending, 0, sizeof(c->pending));
    c->deadline_ms = c->retry_at_ms = 0;
    c->failures = c->exhausted = 0;
}

int alt111_control_init(struct alt111_control *c, unsigned view_count)
{
    if (!c || !view_count || view_count > ALT111_MAX_VIEWS) return ALT111_INVALID;
    memset(c, 0, sizeof(*c));
    c->view_count = view_count;
    c->acknowledged_view = -1;
    return ALT111_OK;
}

uint64_t alt111_control_begin(struct alt111_control *c)
{
    cancel(c);
    ++c->session;
    c->active = 1; c->shown_ack = c->may_be_visible = c->reacquiring = 0;
    c->acknowledged_view = -1;
    c->keyframe_wanted = c->keyframe_done = 0;
    return c->session;
}

int alt111_control_end(struct alt111_control *c, uint64_t session)
{
    if (!c->active || session != c->session) return ALT111_STALE;
    cancel(c);
    c->active = c->shown_ack = c->may_be_visible = c->reacquiring = 0;
    c->acknowledged_view = -1;
    return ALT111_OK;
}

int alt111_control_intent(struct alt111_control *c, unsigned visible, unsigned view)
{
    if (visible > 1 || view >= c->view_count) return ALT111_INVALID;
    if (c->desired == visible && c->desired_view == view) return ALT111_OK;
    /* View changes coalesce behind the existing transaction. A visibility
     * change cancels it; may_be_visible ensures a sent SHOW gets a later STOP. */
    if (c->desired != visible) {
        if (c->pending.type == ALT111_CMD_STOP) c->shown_ack = 0;
        cancel(c);
        c->reacquiring = 0;
    } else if (c->exhausted) {
        cancel(c);
    }
    c->desired = visible; c->desired_view = view;
    return ALT111_OK;
}

int alt111_control_keyframe(struct alt111_control *c, uint64_t session)
{
    if (!c->active || session != c->session) return ALT111_STALE;
    if (!c->desired) return ALT111_WAIT;
    ++c->keyframe_wanted;
    return ALT111_OK;
}

int alt111_control_reacquire(struct alt111_control *c, uint64_t session)
{
    if (!c->active || session != c->session) return ALT111_STALE;
    if (!c->desired) return ALT111_WAIT;
    /*
     * Repeated automatic impulses stay bounded. An explicit reacquire after
     * retry exhaustion is different: it is a new validated/operator ownership
     * event and must be allowed to clear the exhausted transaction.
     */
    if (c->reacquiring && !c->exhausted) return ALT111_BUSY;
    cancel(c);
    c->reacquiring = 1;
    return ALT111_OK;
}

int alt111_control_reshow(struct alt111_control *c, uint64_t session)
{
    if (!c->active || session != c->session) return ALT111_STALE;
    if (!c->desired) return ALT111_WAIT;
    /* A SHOW still due (or in flight) builds its URL when it is sent. */
    if (c->reacquiring || !c->shown_ack) return ALT111_OK;
    /* A pending VIEW/KEYFRAME completes as usual; SHOW follows it. Like an
     * explicit reacquire, this new event may clear an exhausted transaction. */
    if (c->exhausted) cancel(c);
    c->shown_ack = 0;
    return ALT111_OK;
}

int alt111_control_complete(struct alt111_control *c, uint64_t session,
                           uint64_t request, int success, uint64_t now_ms)
{
    struct alt111_command cmd = c->pending;
    if (!c->active || session != c->session || cmd.type == ALT111_CMD_NONE ||
        cmd.request != request) return ALT111_STALE;
    memset(&c->pending, 0, sizeof(c->pending));
    if (now_ms >= c->deadline_ms) success = 0;
    if (!success) {
        ++c->failures;
        c->exhausted = c->failures >= ALT111_ATTEMPTS;
        c->retry_at_ms = after(now_ms, ALT111_RETRY_MS);
        return ALT111_OK;
    }
    c->failures = c->exhausted = 0; c->retry_at_ms = 0;
    switch (cmd.type) {
    case ALT111_CMD_SHOW:
        c->shown_ack = c->may_be_visible = 1;
        c->reacquiring = 0;
        /*
         * A single-view profile already declares initialViewArea=0 in /info.
         * Do not make an unnecessary updateViewArea(0) command gate decoder
         * resynchronization. Multi-view profiles still require an explicit
         * view acknowledgement after SHOW.
         */
        c->acknowledged_view = c->view_count == 1 ? (int)c->desired_view : -1;
        ++c->keyframe_wanted;
        break;
    case ALT111_CMD_STOP:
        c->shown_ack = c->may_be_visible = 0;
        c->acknowledged_view = -1;
        if (c->reacquiring == 1) c->reacquiring = 2;
        break;
    case ALT111_CMD_VIEW: c->acknowledged_view = (int)cmd.view; break;
    case ALT111_CMD_KEYFRAME: c->keyframe_done = cmd.keyframe_demand; break;
    default: break;
    }
    return ALT111_OK;
}

int alt111_control_next(struct alt111_control *c, uint64_t now_ms,
                       struct alt111_command *out)
{
    enum alt111_command_type type = ALT111_CMD_NONE;
    if (!out) return ALT111_INVALID;
    memset(out, 0, sizeof(*out));
    if (!c->active) return ALT111_WAIT;
    if (c->pending.type != ALT111_CMD_NONE) {
        if (now_ms < c->deadline_ms) return ALT111_WAIT;
        (void)alt111_control_complete(c, c->session, c->pending.request, 0, now_ms);
    }
    if (c->exhausted || now_ms < c->retry_at_ms) return ALT111_WAIT;
    if (!c->desired) {
        if (c->may_be_visible) type = ALT111_CMD_STOP;
    } else if (c->reacquiring == 1) type = ALT111_CMD_STOP;
    else if (!c->shown_ack) type = ALT111_CMD_SHOW;
    else if (c->acknowledged_view != (int)c->desired_view) type = ALT111_CMD_VIEW;
    else if (c->keyframe_done != c->keyframe_wanted) type = ALT111_CMD_KEYFRAME;
    if (type == ALT111_CMD_NONE) return ALT111_WAIT;
    c->pending.type = type;
    c->pending.session = c->session;
    c->pending.request = ++c->sequence;
    c->pending.view = c->desired_view;
    c->pending.keyframe_demand = c->keyframe_wanted;
    c->deadline_ms = after(now_ms, ALT111_COMMAND_TIMEOUT_MS);
    if (type == ALT111_CMD_SHOW) c->may_be_visible = 1;
    *out = c->pending;
    return ALT111_OK;
}
