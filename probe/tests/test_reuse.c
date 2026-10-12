/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host tests for the pure reuse helpers: the 3-slot state machine, the
 * kernel-retirement decision function, and the legacy sync-fence ABI decoder.
 * No device, no Vulkan. These assert behaviour (state + decisions + fd
 * lifecycle + ABI decoding), never source text.
 */
#include "reuse.h"
#include "sync_release.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

static int g_pass, g_fail;

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------ retirement decision */

static void test_decide(void)
{
    /* signaled */
    CHECK(tp_release_decide(1, POLLIN, 0, 0, 1, 1) == TP_REL_RETIRED,
          "POLLIN+status=1 must be RETIRED");
    /* not signaled yet */
    CHECK(tp_release_decide(0, 0, 0, 0, 0, 0) == TP_REL_NOT_READY,
          "poll timeout must be NOT_READY");
    /* error bits are never success */
    CHECK(tp_release_decide(1, POLLERR, 0, 0, 1, 1) == TP_REL_FAIL,
          "POLLERR must FAIL");
    CHECK(tp_release_decide(1, POLLHUP, 0, 0, 1, 1) == TP_REL_FAIL,
          "POLLHUP must FAIL");
    CHECK(tp_release_decide(1, POLLNVAL, 0, 0, 1, 1) == TP_REL_FAIL,
          "POLLNVAL must FAIL");
    CHECK(tp_release_decide(1, POLLIN | POLLERR, 0, 0, 1, 1) == TP_REL_FAIL,
          "POLLIN|POLLERR must FAIL (never accept with an error bit)");
    /* readiness without POLLIN */
    CHECK(tp_release_decide(1, 0, 0, 0, 1, 1) == TP_REL_NOT_READY,
          "revents without POLLIN must be NOT_READY");
    /* unsupported / unknown ioctl => fail closed */
    CHECK(tp_release_decide(1, POLLIN, -1, ENOTTY, 0, 0) == TP_REL_FAIL,
          "ENOTTY must FAIL closed");
    CHECK(tp_release_decide(1, POLLIN, -1, EINVAL, 0, 0) == TP_REL_FAIL,
          "ioctl error must FAIL closed");
    /* raw status semantics */
    CHECK(tp_release_decide(1, POLLIN, 0, 0, -1, 1) == TP_REL_FAIL,
          "negative (error) fence status must FAIL");
    CHECK(tp_release_decide(1, POLLIN, 0, 0, 0, 1) == TP_REL_NOT_READY,
          "status 0 (active) must be NOT_READY");
    /* nothing to prove */
    CHECK(tp_release_decide(1, POLLIN, 0, 0, 1, 0) == TP_REL_FAIL,
          "num_fences 0 must FAIL");
    /* poll itself failed */
    CHECK(tp_release_decide(-1, 0, 0, 0, 0, 0) == TP_REL_FAIL,
          "poll error must FAIL");
}

/* Real poll(2) gate: a pipe read end is never a real sync_file, so a POLLIN
 * must fail closed with ENOTTY; an empty pipe must time out at the deadline. */
static void test_poll_info_real(void)
{
    int p[2];
    if (pipe(p) != 0) {
        CHECK(0, "pipe() failed");
        return;
    }
    tp_release_result r;
    double t0 = mono_ms();
    tp_release_verdict v = tp_release_poll_info(p[0], 60, &r);
    double el = mono_ms() - t0;
    CHECK(v == TP_REL_NOT_READY, "empty pipe fence: verdict=%d expected NOT_READY", v);
    CHECK(el >= 30.0, "poll gate returned before its budget (%.1fms)", el);
    CHECK(el < 500.0, "poll gate overran its budget (%.1fms)", el);

    char c = 'x';
    if (write(p[1], &c, 1) != 1)
        CHECK(0, "pipe write failed");
    v = tp_release_poll_info(p[0], 60, &r);
    CHECK(v == TP_REL_FAIL, "POLLIN on non-sync fd: verdict=%d expected FAIL", v);
    CHECK(r.info_rc != 0, "POLLIN on non-sync fd: info_rc=%d expected error", r.info_rc);
    CHECK(r.info_errno == ENOTTY, "info_errno=%d expected ENOTTY", r.info_errno);
    CHECK(r.api == TP_SYNC_API_LEGACY,
          "api=%d expected LEGACY after modern ENOTTY", r.api);
    CHECK(r.legacy_rc != 0 && r.legacy_errno == ENOTTY,
          "legacy fallback not attempted/ENOTTY (rc=%d errno=%d)",
          r.legacy_rc, r.legacy_errno);
    CHECK((r.revents & POLLIN) != 0, "revents=0x%04x expected POLLIN", (unsigned)r.revents);

    close(p[0]);
    close(p[1]);
}

/* ------------------------------------------------ legacy sync ABI decode */

static void put_hdr(unsigned char *buf, uint32_t total, int32_t status)
{
    struct tp_sync_fence_info_data h;
    memset(&h, 0, sizeof(h));
    h.len = total;
    h.status = status;
    memcpy(buf, &h, sizeof(h));
}

static void put_pt(unsigned char *buf, unsigned off, uint32_t len,
                   int32_t status, const char *drv)
{
    struct tp_sync_pt_info p;
    memset(&p, 0, sizeof(p));
    p.len = len;
    p.status = status;
    if (drv)
        snprintf(p.driver_name, sizeof(p.driver_name), "%s", drv);
    memcpy(buf + off, &p, sizeof(p));
}

/* Fold the decoder rc into a verdict the way the real gate does: a rejected
 * (malformed/inconsistent) reply fails closed rather than being classified. */
static tp_release_verdict decode_verdict(const unsigned char *buf, unsigned len,
                                         tp_legacy_decoded *d)
{
    if (tp_legacy_decode(buf, len, d) != 0)
        return TP_REL_FAIL;
    return tp_release_decide(1, POLLIN, 0, 0, d->status, d->num_points);
}

/* Legacy SYNC_IOC_FENCE_INFO reply decoding: aggregate status and a non-empty,
 * well-formed sync_pt_info chain; malformed/truncated/empty/negative fail. */
static void test_legacy_decode(void)
{
    unsigned char buf[TP_LEGACY_INFO_MAX];
    tp_legacy_decoded d;

    /* signaled, one nvhost-like point (80 + 8 driver data = 88) */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88, 1);
    put_pt(buf, 40, 88, 1, "nvhost_sync");
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == 0 && d.status == 1 &&
          d.num_points == 1 && d.bad == 0,
          "legacy signaled/1pt not decoded (status=%d n=%u)", d.status, d.num_points);
    CHECK(tp_release_decide(1, POLLIN, 0, 0, d.status, d.num_points) == TP_REL_RETIRED,
          "legacy signaled must RETIRE");

    /* active (status 0) decodes but is NOT_READY */
    put_hdr(buf, 40 + 88, 0);
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == 0 && d.status == 0 &&
          d.num_points == 1, "legacy active not decoded");
    CHECK(tp_release_decide(1, POLLIN, 0, 0, d.status, d.num_points) == TP_REL_NOT_READY,
          "legacy active must be NOT_READY");

    /* negative aggregate status decodes but FAILS closed */
    put_hdr(buf, 40 + 88, -5);
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == 0 && d.status == -5,
          "legacy negative aggregate not decoded");
    CHECK(tp_release_decide(1, POLLIN, 0, 0, d.status, d.num_points) == TP_REL_FAIL,
          "legacy negative status must FAIL");

    /* negative point status -> reject */
    put_hdr(buf, 40 + 88, 1);
    put_pt(buf, 40, 88, -1, "nvhost_sync");
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1 && d.bad == 1,
          "legacy negative point status accepted");

    /* two well-formed points */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88 + 88, 1);
    put_pt(buf, 40, 88, 1, "nvhost_sync");
    put_pt(buf, 128, 88, 1, "nvhost_sync");
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == 0 && d.num_points == 2,
          "legacy 2 points not decoded (n=%u)", d.num_points);

    /* malformed lengths / empty / truncated -> reject */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 39, 1);                       /* total < header */
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1, "total<40 accepted");
    put_hdr(buf, 40, 1);                       /* header only: no points */
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1, "empty points accepted");
    put_hdr(buf, 40 + 79, 1);                  /* record len < 80 */
    put_pt(buf, 40, 79, 1, "x");
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1, "short record len accepted");
    put_hdr(buf, 40 + 88, 1);
    put_pt(buf, 40, 999, 1, "x");              /* record overruns reply */
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1, "overrun record accepted");
    put_hdr(buf, 40 + 88, 1);
    put_pt(buf, 40, 88, 1, "x");
    CHECK(tp_legacy_decode(buf, 8, &d) == -1, "truncated buffer accepted");
    CHECK(tp_legacy_decode(buf, 40 + 88 - 1, &d) == -1, "total>len accepted");
    CHECK(tp_legacy_decode(NULL, sizeof(buf), &d) == -1, "NULL buffer accepted");

    /* zeroed header (len 0) -> reject */
    memset(buf, 0, sizeof(buf));
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1 && d.status == 0,
          "zero header accepted");

    /* --- M3g2 fail-closed status-domain review ---------------------------
     * The kernel emits only 0 active / 1 signaled / <0 error.  A signaled
     * aggregate (1) requires every point signaled; unknown values, and the
     * aggregate1+point0 contradiction, must be rejected.  aggregate0/point1
     * is tolerated but must never retire.  decode_verdict() folds the decoder
     * rc into a verdict (rejected -> FAIL). */

    /* aggregate1 + point0 (signaled aggregate with an active point) */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88, 1);
    put_pt(buf, 40, 88, 0, "nvhost_sync");
    CHECK(tp_legacy_decode(buf, sizeof(buf), &d) == -1 && d.status == 1,
          "aggregate1+point0 not rejected (status=%d)", d.status);
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_FAIL,
          "aggregate1+point0 must FAIL closed, never RETIRE");

    /* aggregate1 + point2 (unknown point status) */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88, 1);
    put_pt(buf, 40, 88, 2, "nvhost_sync");
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_FAIL,
          "aggregate1+point2 (unknown point status) must FAIL closed");

    /* aggregate2 + point1 (unknown aggregate status) */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88, 2);
    put_pt(buf, 40, 88, 1, "nvhost_sync");
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_FAIL,
          "aggregate2+point1 (unknown aggregate status) must FAIL closed");
    CHECK(d.status == 2, "aggregate2 raw status not preserved (status=%d)", d.status);

    /* valid two-signaled: aggregate1 + points {1,1} */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88 + 88, 1);
    put_pt(buf, 40, 88, 1, "nvhost_sync");
    put_pt(buf, 128, 88, 1, "nvhost_sync");
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_RETIRED &&
          d.num_points == 2,
          "valid two-signaled must RETIRE (n=%u)", d.num_points);

    /* aggregate0 + point1 (may be transient aggregate lag) */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88, 0);
    put_pt(buf, 40, 88, 1, "nvhost_sync");
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_NOT_READY,
          "aggregate0+point1 must be NOT_READY (never retire)");

    /* mixed points: aggregate1 + {1,0} contradiction */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88 + 88, 1);
    put_pt(buf, 40, 88, 1, "nvhost_sync");
    put_pt(buf, 128, 88, 0, "nvhost_sync");
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_FAIL,
          "aggregate1 mixed {1,0} points must FAIL closed");

    /* mixed points: aggregate0 + {0,1} lag */
    memset(buf, 0, sizeof(buf));
    put_hdr(buf, 40 + 88 + 88, 0);
    put_pt(buf, 40, 88, 0, "nvhost_sync");
    put_pt(buf, 128, 88, 1, "nvhost_sync");
    CHECK(decode_verdict(buf, sizeof(buf), &d) == TP_REL_NOT_READY &&
          d.num_points == 2,
          "aggregate0 mixed {0,1} points must be NOT_READY (n=%u)", d.num_points);
}

/* Legacy query is allowed ONLY as a fallback from a modern ENOTTY. */
static void test_legacy_policy(void)
{
    CHECK(tp_release_should_try_legacy(0, 0) == 0, "legacy tried after modern success");
    CHECK(tp_release_should_try_legacy(-1, ENOTTY) == 1, "ENOTTY did not select legacy");
    CHECK(tp_release_should_try_legacy(-1, EINVAL) == 0, "EINVAL must not select legacy");
    CHECK(tp_release_should_try_legacy(-1, EFAULT) == 0, "EFAULT must not select legacy");
    CHECK(tp_release_should_try_legacy(-1, EACCES) == 0, "EACCES must not select legacy");
    CHECK(tp_release_should_try_legacy(-1, 0) == 0, "error+errno0 must not select legacy");
    CHECK(strcmp(tp_sync_api_name(TP_SYNC_API_LEGACY), "legacy") == 0 &&
          strcmp(tp_sync_api_name(TP_SYNC_API_MODERN), "modern") == 0 &&
          strcmp(tp_sync_api_name(TP_SYNC_API_NONE), "none") == 0,
          "api names wrong");
}

/* --------------------------------------------------------- state machine */

static void test_init(void)
{
    tp_reuse r;
    tp_reuse_init(&r);
    CHECK(r.n_slots == TP_REUSE_SLOTS, "n_slots=%d expected %d", r.n_slots, TP_REUSE_SLOTS);
    CHECK(r.last_submitted == -1, "last_submitted=%d expected -1", r.last_submitted);
    CHECK(tp_reuse_live_fds(&r) == 0, "fresh ring has live fds");
    for (int i = 0; i < TP_REUSE_SLOTS; i++) {
        CHECK(r.slot[i].state == TP_SLOT_UNUSED, "slot %d not UNUSED", i);
        CHECK(r.slot[i].post_fd == -1, "slot %d post_fd=%d expected -1", i, r.slot[i].post_fd);
    }
}

/* The first use of every slot is local-owned (no acquire); after a validated
 * retirement the slot requires an acquire before its next write. */
static void test_roundrobin_and_acquire(void)
{
    tp_reuse r;
    tp_reuse_init(&r);
    for (int k = 0; k < 6; k++) {
        int slot = -1, acq = -1;
        if (k >= TP_REUSE_SLOTS) {
            int prev = k % TP_REUSE_SLOTS; /* == k - 3 with 3 slots */
            CHECK(r.slot[prev].state == TP_SLOT_SUBMITTED,
                  "frame %d: expected slot %d SUBMITTED", k, prev);
            CHECK(tp_reuse_mark_retired(&r, prev) == 0, "frame %d retire failed", k);
        }
        CHECK(tp_reuse_begin(&r, &slot, &acq) == 0, "frame %d begin failed", k);
        CHECK(slot == k % TP_REUSE_SLOTS, "frame %d slot=%d expected %d",
              k, slot, k % TP_REUSE_SLOTS);
        int want_acq = (k >= TP_REUSE_SLOTS) ? 1 : 0;
        CHECK(acq == want_acq, "frame %d acquire=%d expected %d", k, acq, want_acq);
        CHECK(r.slot[slot].state != TP_SLOT_SUBMITTED, "frame %d reused a live slot", k);
        CHECK(tp_reuse_live_fds(&r) <= TP_REUSE_SLOTS,
              "frame %d live fds %d > %d", k, tp_reuse_live_fds(&r), TP_REUSE_SLOTS);
        CHECK(tp_reuse_mark_submitted(&r, slot, 100 + k) == 0,
              "frame %d submit failed", k);
        CHECK(tp_reuse_live_fds(&r) <= TP_REUSE_SLOTS,
              "frame %d live fds after submit %d", k, tp_reuse_live_fds(&r));
    }
    CHECK(r.submitted == 6, "submitted=%u expected 6", r.submitted);
    CHECK(r.reused == 3, "reused=%u expected 3", r.reused);
    CHECK(r.retired == 3, "retired=%u expected 3", r.retired);
}

/* A slot whose own fence was NOT retired must never be picked again: the
 * overwrite would race the still-outstanding kernel fence. */
static void test_no_overwrite_before_retire(void)
{
    tp_reuse r;
    tp_reuse_init(&r);
    for (int k = 0; k < TP_REUSE_SLOTS; k++) {
        int slot = -1, acq = 0;
        CHECK(tp_reuse_begin(&r, &slot, &acq) == 0, "setup frame %d begin", k);
        CHECK(tp_reuse_mark_submitted(&r, slot, 100 + k) == 0, "setup frame %d submit", k);
    }
    int slot = -1, acq = 0;
    CHECK(tp_reuse_begin(&r, &slot, &acq) == -1,
          "begin picked an unretired slot (slot=%d)", slot);
    CHECK(r.errors >= 1, "guard refusal was not counted");

    /* Explicit double-submit / double-retire guards. */
    CHECK(tp_reuse_mark_submitted(&r, 0, 999) == -1,
          "submit into an unretired slot accepted");
    CHECK(tp_reuse_mark_retired(&r, 0) == 0, "valid retire rejected");
    CHECK(tp_reuse_mark_retired(&r, 0) == -1, "double retire accepted");
    CHECK(tp_reuse_mark_submitted(&r, 0, -1) == -1, "negative post_fd accepted");
    CHECK(tp_reuse_mark_retired(&r, 0) == -1, "retire of a non-SUBMITTED slot accepted");
}

/* Non-latest guard: the round-robin pick must never equal the most recently
 * submitted slot (it is released BY the next flip). */
static void test_non_latest_guard(void)
{
    tp_reuse r;
    tp_reuse_init(&r);
    int slot = -1, acq = 0;
    CHECK(tp_reuse_begin(&r, &slot, &acq) == 0, "begin 0 failed");
    CHECK(tp_reuse_mark_submitted(&r, slot, 100) == 0, "submit 0 failed");
    r.n_slots = 1;             /* pathological ring: next pick == last slot */
    CHECK(tp_reuse_begin(&r, &slot, &acq) == -1, "non-latest guard accepted");
    r.n_slots = TP_REUSE_SLOTS;
    /* a consistent ring proceeds normally again */
    CHECK(tp_reuse_begin(&r, &slot, &acq) == 0, "ring did not recover");
    CHECK(slot != r.last_submitted, "begin returned the most recent slot");
}

/* Every retained fd is closed exactly once across a full run (retired in the
 * loop, remainder at teardown). */
static void test_fd_close_once(void)
{
    tp_reuse r;
    tp_reuse_init(&r);
    int seen[256];
    memset(seen, 0, sizeof(seen));
    int closed = 0;
    unsigned frames = 12;
    for (unsigned k = 0; k < frames; k++) {
        int prev = (int)(k % TP_REUSE_SLOTS);
        if (r.slot[prev].state == TP_SLOT_SUBMITTED) {
            int fd = r.slot[prev].post_fd;
            CHECK(tp_reuse_mark_retired(&r, prev) == 0, "retire at %u", k);
            CHECK(fd >= 0 && seen[fd] == 0, "fd %d closed twice at %u", fd, k);
            seen[fd] = 1;
            closed++;
        }
        int slot = -1, acq = 0;
        CHECK(tp_reuse_begin(&r, &slot, &acq) == 0, "begin at %u", k);
        CHECK(tp_reuse_mark_submitted(&r, slot, 100 + (int)k) == 0, "submit at %u", k);
        CHECK(tp_reuse_live_fds(&r) <= TP_REUSE_SLOTS, "live>3 at %u", k);
    }
    for (int i = 0; i < TP_REUSE_SLOTS; i++) {
        if (r.slot[i].state == TP_SLOT_SUBMITTED) {
            int fd = r.slot[i].post_fd;
            CHECK(fd >= 0 && seen[fd] == 0, "pending fd %d closed twice", fd);
            seen[fd] = 1;
            closed++;
        }
    }
    CHECK(closed == (int)frames, "closed %d fds expected %u", closed, frames);
    CHECK(r.submitted == frames, "submitted=%u expected %u", r.submitted, frames);
    CHECK(r.reused == frames - TP_REUSE_SLOTS, "reused=%u expected %u",
          r.reused, frames - TP_REUSE_SLOTS);
}

int main(void)
{
    test_decide();
    test_poll_info_real();
    test_legacy_decode();
    test_legacy_policy();
    test_init();
    test_roundrobin_and_acquire();
    test_no_overwrite_before_retire();
    test_non_latest_guard();
    test_fd_close_once();
    printf("reuse tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
