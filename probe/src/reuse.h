/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded 3-slot scanout-buffer reuse state machine (pure, host-testable).
 *
 * This helper owns only bookkeeping and the fail-closed guards; it performs no
 * I/O. The driver polls the slot's own retained post_syncpt_fd and, after a
 * validated kernel retirement, calls tp_reuse_mark_retired()/mark_submitted().
 *
 * Invariants:
 *   - a slot is never picked while its own post fence is still SUBMITTED
 *     (the fence gate must pass BEFORE any GPU write to that slot);
 *   - the round-robin slot is never the most recently submitted slot;
 *   - live_post_fds <= n_slots at all times;
 *   - the first use of a slot is local-owned (no EXTERNAL acquire); only a
 *     previously released slot reports needs_acquire.
 */
#ifndef TP_REUSE_H
#define TP_REUSE_H

#include "tp.h"

typedef enum {
    TP_SLOT_UNUSED    = 0,  /* never submitted; local-owned; first use       */
    TP_SLOT_SUBMITTED = 1,  /* owns a live post fence fd, awaiting retirement */
    TP_SLOT_AVAILABLE = 2   /* retired once; acquire before the next write   */
} tp_slot_state;

typedef struct {
    int      state;      /* tp_slot_state                                  */
    int      post_fd;    /* retained own post fence fd, -1 when none       */
    unsigned flips;      /* successful submissions into this slot          */
} tp_reuse_slot;

typedef struct {
    tp_reuse_slot slot[TP_REUSE_SLOTS];
    int      n_slots;
    int      frame_seq;       /* frames begun (== next frame sequence)     */
    int      last_submitted;  /* slot of the most recent submission, -1    */
    int      live_post_fds;   /* slots currently SUBMITTED                 */
    unsigned submitted;
    unsigned reused;          /* submissions into an already-used slot     */
    unsigned retired;
    unsigned errors;          /* guard/fence-gate refusals                 */
} tp_reuse;

void tp_reuse_init(tp_reuse *r);

/* The round-robin slot for the next frame (frame_seq % n_slots), no side
 * effects. The driver uses it to locate the slot whose own fence must be gated
 * to completion BEFORE tp_reuse_begin() re-validates and hands it out. */
int tp_reuse_peek_slot(const tp_reuse *r);

/* Choose the slot for the next frame (frame_seq % n_slots).
 * Returns 0, storing the slot and whether an EXTERNAL->compute acquire is
 * required before any GPU write. Returns -1 (fail closed) if the slot is the
 * most recently submitted slot or is still SUBMITTED. */
int tp_reuse_begin(tp_reuse *r, int *slot_out, int *needs_acquire);

/* Record a validated retirement of the slot's own post fence: clears the fd
 * (so the driver can close it exactly once) and marks the slot AVAILABLE. */
int tp_reuse_mark_retired(tp_reuse *r, int slot);

/* Record a successful submission plus the new retained post fence fd. */
int tp_reuse_mark_submitted(tp_reuse *r, int slot, int post_fd);

/* Count of retained post fence fds (== slots in SUBMITTED). */
int tp_reuse_live_fds(const tp_reuse *r);

#endif /* TP_REUSE_H */
