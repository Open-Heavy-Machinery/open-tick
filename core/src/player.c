/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file player.c
 * @brief Scenario player. See player.h.
 *
 * Extracted from open-crank v0.1 (apply_due_events and
 * oc_bench_scenario_done in sim/runtime/src/bench.c).
 */

#include <opentick/player.h>

void otk_player_init(OtkPlayer *p, const OtkScenario *scenario, OtkApplyFn apply, void *ctx) {
    p->scenario = scenario;
    p->next = 0U;
    p->apply = apply;
    p->ctx = ctx;
}

void otk_player_advance(OtkPlayer *p, OtkNs now) {
    const OtkScenario *s = p->scenario;
    if (s == NULL) {
        return;
    }
    while (p->next < s->n && s->events[p->next].t <= now) {
        const OtkEvent *e = &s->events[p->next];
        if (e->signal != OTK_SIGNAL_END && p->apply != NULL) {
            p->apply(p->ctx, e->signal, e->value);
        }
        p->next++;
    }
}

bool otk_player_done(const OtkPlayer *p, OtkNs now) {
    return p->scenario != NULL && p->scenario->has_end && now >= p->scenario->end;
}
