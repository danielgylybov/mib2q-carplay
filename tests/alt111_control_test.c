/*
 * altscreen_hook control core: session start sequence and the suggestUI reshow
 * (issue #35: the app taking over the cluster mid-session needs a new showUI).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "alt111.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static uint64_t now_ms = 1000;

/* Next command, acknowledged at once; NONE when the controller waits. */
static enum alt111_command_type step(struct alt111_control *c, int success)
{
    struct alt111_command cmd;
    if (alt111_control_next(c, now_ms, &cmd) != ALT111_OK) return ALT111_CMD_NONE;
    alt111_control_complete(c, cmd.session, cmd.request, success, now_ms + 10);
    now_ms += 20;
    return cmd.type;
}

static void settle(struct alt111_control *c)
{
    int n;
    for (n = 0; n < 8 && step(c, 1) != ALT111_CMD_NONE; ++n) {}
}

int main(void)
{
    struct alt111_control c;
    struct alt111_command cmd;
    uint64_t s;
    char url[160];

    CHECK(alt111_eta_disabled_url(
              "maps:/car/instrumentcluster?showETA=yes&showSpeedLimit=no",
              url, sizeof(url)) == ALT111_OK,
          "ETA transition URL accepted");
    CHECK(strcmp(url,
              "maps:/car/instrumentcluster?showETA=no&showSpeedLimit=no") == 0,
          "ETA transition changes only yes to no");
    CHECK(alt111_eta_disabled_url(
              "maps:/car/instrumentcluster?showSpeedLimit=no&showETA=yes",
              url, sizeof(url)) == ALT111_OK &&
          strcmp(url,
              "maps:/car/instrumentcluster?showSpeedLimit=no&showETA=no") == 0,
          "ETA transition handles final query item");
    CHECK(alt111_eta_disabled_url(
              "maps:/car/instrumentcluster?showETA=no&showSpeedLimit=no",
              url, sizeof(url)) == ALT111_INVALID,
          "ETA OFF does not start a transition");
    CHECK(alt111_eta_disabled_url(
              "maps:/car/instrumentcluster?notshowETA=yes&showSpeedLimit=no",
              url, sizeof(url)) == ALT111_INVALID,
          "ETA transition rejects a substring key");
    CHECK(alt111_eta_disabled_url(
              "maps:/car/instrumentcluster?showETA=yes&showSpeedLimit=no",
              url, 8) == ALT111_NOMEM,
          "ETA transition fails closed for a short buffer");

    CHECK(alt111_control_init(&c, 3) == ALT111_OK, "init");
    s = alt111_control_begin(&c);
    CHECK(alt111_control_reshow(&c, s) == ALT111_WAIT, "reshow before projection waits");
    alt111_control_intent(&c, 1, 1);

    /* Session start: SHOW, VIEW, KEYFRAME, then idle. */
    CHECK(step(&c, 1) == ALT111_CMD_SHOW, "start: SHOW");
    CHECK(step(&c, 1) == ALT111_CMD_VIEW, "start: VIEW");
    CHECK(step(&c, 1) == ALT111_CMD_KEYFRAME, "start: KEYFRAME");
    CHECK(step(&c, 1) == ALT111_CMD_NONE, "start: idle");

    /* suggestUI: the same sequence again, with no STOP first. */
    CHECK(alt111_control_reshow(&c, s) == ALT111_OK, "reshow accepted");
    CHECK(step(&c, 1) == ALT111_CMD_SHOW, "reshow: SHOW");
    CHECK(step(&c, 1) == ALT111_CMD_VIEW, "reshow: VIEW");
    CHECK(step(&c, 1) == ALT111_CMD_KEYFRAME, "reshow: KEYFRAME");
    CHECK(step(&c, 1) == ALT111_CMD_NONE, "reshow: idle");

    /* A burst coalesces into one SHOW. */
    alt111_control_reshow(&c, s);
    alt111_control_reshow(&c, s);
    CHECK(step(&c, 1) == ALT111_CMD_SHOW, "burst: one SHOW");
    CHECK(step(&c, 1) == ALT111_CMD_VIEW, "burst: VIEW, no second SHOW");
    settle(&c);

    /* A VIEW in flight completes first; SHOW follows. */
    alt111_control_intent(&c, 1, 0);
    CHECK(alt111_control_next(&c, now_ms, &cmd) == ALT111_OK && cmd.type == ALT111_CMD_VIEW,
          "view change: VIEW in flight");
    CHECK(alt111_control_reshow(&c, s) == ALT111_OK, "reshow during VIEW");
    CHECK(alt111_control_complete(&c, cmd.session, cmd.request, 1, now_ms + 10) == ALT111_OK,
          "in-flight VIEW still completes");
    now_ms += 20;
    CHECK(step(&c, 1) == ALT111_CMD_SHOW, "SHOW after the VIEW");
    settle(&c);
    CHECK(c.acknowledged_view == 0, "desired view acknowledged after reshow");

    /* Retries exhausted on a VIEW: a reshow starts a fresh transaction. */
    alt111_control_intent(&c, 1, 2);
    CHECK(step(&c, 0) == ALT111_CMD_VIEW, "VIEW fails 1");
    now_ms += ALT111_RETRY_MS;
    CHECK(step(&c, 0) == ALT111_CMD_VIEW, "VIEW fails 2");
    now_ms += ALT111_RETRY_MS;
    CHECK(step(&c, 0) == ALT111_CMD_VIEW, "VIEW fails 3");
    now_ms += ALT111_RETRY_MS;
    CHECK(step(&c, 1) == ALT111_CMD_NONE, "exhausted: idle");
    CHECK(alt111_control_reshow(&c, s) == ALT111_OK, "reshow after exhaustion");
    CHECK(step(&c, 1) == ALT111_CMD_SHOW, "exhausted: SHOW again");
    settle(&c);

    /* Reacquire (STOP then SHOW) already ends in a SHOW: reshow adds nothing. */
    CHECK(alt111_control_reacquire(&c, s) == ALT111_OK, "reacquire");
    CHECK(alt111_control_reshow(&c, s) == ALT111_OK, "reshow during reacquire");
    CHECK(step(&c, 1) == ALT111_CMD_STOP, "reacquire: STOP");
    CHECK(step(&c, 1) == ALT111_CMD_SHOW, "reacquire: SHOW");
    CHECK(step(&c, 1) == ALT111_CMD_VIEW, "reacquire: VIEW, no extra SHOW");
    settle(&c);

    /* Stale or ended sessions are refused. */
    CHECK(alt111_control_reshow(&c, s + 1) == ALT111_STALE, "other session stale");
    alt111_control_intent(&c, 0, 0);
    settle(&c);
    alt111_control_end(&c, s);
    CHECK(alt111_control_reshow(&c, s) == ALT111_STALE, "ended session stale");

    if (failures) return 1;
    printf("OK\n");
    return 0;
}
