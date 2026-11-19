/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file player.h
 * @brief Plays a scenario against a machine: hands each event to the
 *        application when its time comes.
 *
 * The player never reads a clock. The machine tells it what time it is at
 * the start of each step; every event whose time is at or before that time
 * and has not been applied yet is applied, in file order. An event that
 * falls inside a step therefore applies at the start of the next step:
 *
 *     step starts at  0 ms   10 ms   20 ms
 *     event at 15 ms   -       -     applied
 *
 * The `end` event is never handed to the application; it only makes
 * otk_player_done() true.
 */

#ifndef OPENTICK_PLAYER_H
#define OPENTICK_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

#include <opentick/scenario.h>
#include <opentick/timebase.h>

/** @brief Applies one event to the application's signals. */
typedef void (*OtkApplyFn)(void *ctx, OtkSignalId signal, double value);

typedef struct {
    const OtkScenario *scenario; /**< may be NULL: nothing to play */
    size_t next;                 /**< index of the next event to apply */
    OtkApplyFn apply;
    void *ctx;
} OtkPlayer;

/** @brief Sets up a player. @p scenario may be NULL. */
void otk_player_init(OtkPlayer *p, const OtkScenario *scenario, OtkApplyFn apply, void *ctx);

/** @brief Applies every event due at or before @p now that has not been applied. */
void otk_player_advance(OtkPlayer *p, OtkNs now);

/** @brief True once the scenario has an `end` and @p now has reached it. */
bool otk_player_done(const OtkPlayer *p, OtkNs now);

#endif /* OPENTICK_PLAYER_H */
