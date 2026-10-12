/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "reuse.h"

#include <string.h>

void tp_reuse_init(tp_reuse *r)
{
    memset(r, 0, sizeof(*r));
    r->n_slots = TP_REUSE_SLOTS;
    r->last_submitted = -1;
    for (int i = 0; i < TP_REUSE_SLOTS; i++) {
        r->slot[i].state = TP_SLOT_UNUSED;
        r->slot[i].post_fd = -1;
    }
}

int tp_reuse_live_fds(const tp_reuse *r)
{
    int n = 0;
    for (int i = 0; i < r->n_slots; i++)
        if (r->slot[i].state == TP_SLOT_SUBMITTED)
            n++;
    return n;
}

int tp_reuse_peek_slot(const tp_reuse *r)
{
    if (r->n_slots <= 0)
        return -1;
    return r->frame_seq % r->n_slots;
}

int tp_reuse_begin(tp_reuse *r, int *slot_out, int *needs_acquire)
{
    if (r->n_slots <= 0)
        return -1;
    int s = r->frame_seq % r->n_slots;

    /* Never pick the most recently submitted slot: it is released BY the next
     * flip, so waiting on it here would deadlock. */
    if (s == r->last_submitted) {
        r->errors++;
        return -1;
    }
    /* The slot's own fence gate must have passed before we touch it. */
    if (r->slot[s].state == TP_SLOT_SUBMITTED) {
        r->errors++;
        return -1;
    }
    if (r->slot[s].state != TP_SLOT_UNUSED &&
        r->slot[s].state != TP_SLOT_AVAILABLE) {
        r->errors++;
        return -1;
    }

    if (slot_out)
        *slot_out = s;
    if (needs_acquire)
        *needs_acquire = (r->slot[s].state == TP_SLOT_AVAILABLE) ? 1 : 0;
    return 0;
}

int tp_reuse_mark_retired(tp_reuse *r, int slot)
{
    if (slot < 0 || slot >= r->n_slots)
        return -1;
    if (r->slot[slot].state != TP_SLOT_SUBMITTED)
        return -1;
    if (r->slot[slot].post_fd < 0)
        return -1;
    r->slot[slot].post_fd = -1;
    r->slot[slot].state = TP_SLOT_AVAILABLE;
    r->retired++;
    r->live_post_fds = tp_reuse_live_fds(r);
    return 0;
}

int tp_reuse_mark_submitted(tp_reuse *r, int slot, int post_fd)
{
    if (slot < 0 || slot >= r->n_slots)
        return -1;
    if (post_fd < 0)
        return -1;
    if (r->slot[slot].state == TP_SLOT_SUBMITTED)
        return -1; /* would overwrite an unretired slot */

    int was_used = (r->slot[slot].flips > 0);
    r->slot[slot].flips++;
    r->slot[slot].post_fd = post_fd;
    r->slot[slot].state = TP_SLOT_SUBMITTED;
    r->last_submitted = slot;
    r->frame_seq++;
    r->submitted++;
    if (was_used)
        r->reused++;
    r->live_post_fds = tp_reuse_live_fds(r);
    return 0;
}
