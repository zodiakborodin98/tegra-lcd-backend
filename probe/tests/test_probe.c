/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host lifecycle + CLI tests for tegra-present-probe.
 *
 * main.c is compiled into this binary with -Dmain=tp_real_main (see Makefile);
 * src/gpu.c and src/dc.c are replaced by tests/stub_gpu.c + tests/stub_dc.c.
 * No Vulkan/device is touched. These are behavioural tests (return status and
 * teardown ordering), not source greps.
 */
#include "tp.h"
#include "tpstub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int tp_real_main(int argc, char **argv);

static int g_pass, g_fail;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

/* --------------------------------------------------------------- CLI tests */

static int parse_rc(char **argv, int argc, tp_config *c)
{
    tp_config_defaults(c);
    return tp_config_parse(c, argc, argv);
}

static void test_cli(void)
{
    tp_config c;

    {
        char *v[] = { "p", "--present", "--dc", "0", "--window", "5",
                      "--mode", "1280x720", "--physical", "720x1280",
                      "--orientation", "90", "--duration", "1" };
        CHECK(parse_rc(v, N(v), &c) == 0, "baseline valid CLI rejected");
    }
    {
        char *v[] = { "p", "--mode", "1280x720x" };
        CHECK(parse_rc(v, N(v), &c) == -1, "mode trailing junk accepted");
    }
    {
        char *v[] = { "p", "--mode", "1280x720abc" };
        CHECK(parse_rc(v, N(v), &c) == -1, "mode alpha junk accepted");
    }
    {
        char *v[] = { "p", "--mode", "0x720" };
        CHECK(parse_rc(v, N(v), &c) == -1, "zero width accepted");
    }
    {
        char *v[] = { "p", "--mode", "4097x720" };
        CHECK(parse_rc(v, N(v), &c) == -1, "logical >4096 accepted");
    }
    {
        char *v[] = { "p", "--mode", "999999999999x720" };
        CHECK(parse_rc(v, N(v), &c) == -1, "overflowing int accepted");
    }
    {
        char *v[] = { "p", "--physical", "7x0" };
        CHECK(parse_rc(v, N(v), &c) == -1, "zero physical accepted");
    }
    {
        /* logical 1280x720 @ 90 => physical must be 720x1280. */
        char *v[] = { "p", "--mode", "1280x720", "--physical", "1280x720",
                      "--orientation", "90" };
        CHECK(parse_rc(v, N(v), &c) == -1, "orientation-inconsistent physical accepted");
    }
    {
        char *v[] = { "p", "--duration", "0.05" };
        CHECK(parse_rc(v, N(v), &c) == -1, "duration <0.1 accepted");
    }
    {
        char *v[] = { "p", "--duration", "30.1" };
        CHECK(parse_rc(v, N(v), &c) == -1, "duration >30 accepted");
    }
    {
        char *v[] = { "p", "--duration", "nan" };
        CHECK(parse_rc(v, N(v), &c) == -1, "non-finite duration accepted");
    }
    {
        char *v[] = { "p", "--duration", "inf" };
        CHECK(parse_rc(v, N(v), &c) == -1, "infinite duration accepted");
    }
    {
        char *v[] = { "p", "--duration", "2s" };
        CHECK(parse_rc(v, N(v), &c) == -1, "duration junk accepted");
    }
    {
        char *v[] = { "p", "--dc", "4" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--dc 4 accepted");
    }
    {
        char *v[] = { "p", "--dc", "-2" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--dc -2 accepted");
    }
    {
        char *v[] = { "p", "--dc", "-1" };
        CHECK(parse_rc(v, N(v), &c) == 0, "--dc -1 (autodetect) rejected");
    }
    {
        char *v[] = { "p", "--window", "6" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--window 6 accepted");
    }
    {
        char *v[] = { "p", "--frame-count", "0" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--frame-count 0 accepted");
    }
    {
        char *v[] = { "p", "--frame-count", "9" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.frame_count == TP_MAX_SCANOUT_BUFFERS,
              "--frame-count 9 not clamped to %d", TP_MAX_SCANOUT_BUFFERS);
    }
    {
        char *v[] = { "p", "--block-height-log2", "0" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--block-height-log2 0 accepted");
    }
    {
        /* The per-frame period must be duration/frames with no 2 s/frame cap. */
        char *v[] = { "p", "--duration", "20", "--frame-count", "4" };
        CHECK(parse_rc(v, N(v), &c) == 0 && fabs(tp_frame_period(&c) - 5.0) < 1e-9,
              "period(20s,4) = %.4f expected 5.0", tp_frame_period(&c));
    }
    {
        char *v[] = { "p", "--duration", "0.4", "--frame-count", "4" };
        CHECK(parse_rc(v, N(v), &c) == 0 && fabs(tp_frame_period(&c) - 0.1) < 1e-9,
              "period(0.4s,4) = %.4f expected 0.1", tp_frame_period(&c));
    }
    {
        char *v[] = { "p", "--duration", "30", "--frame-count", "1" };
        CHECK(parse_rc(v, N(v), &c) == 0 && fabs(tp_frame_period(&c) - 30.0) < 1e-9,
              "period(30s,1) = %.4f expected 30.0", tp_frame_period(&c));
    }
    /* --enable-head is a bounded prototype: present + --dc 0 + --window 0. */
    {
        char *v[] = { "p", "--present", "--enable-head", "--dc", "0" };
        CHECK(parse_rc(v, N(v), &c) == 0, "valid --enable-head rejected");
    }
    {
        char *v[] = { "p", "--enable-head" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--enable-head without --present accepted");
    }
    {
        char *v[] = { "p", "--diagnostic-only", "--enable-head" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--enable-head with --diagnostic-only accepted");
    }
    {
        char *v[] = { "p", "--present", "--enable-head", "--dc", "1", "--window", "0" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--enable-head with --dc 1 accepted");
    }
    {
        char *v[] = { "p", "--present", "--enable-head", "--dc", "0", "--window", "1" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--enable-head with --window 1 accepted");
    }

    /* --reuse opt-in: strict bounds, defaults, and default-mode invariance. */
    {
        char *v[] = { "p", "--present" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.reuse == 0 && c.frame_count == 4 &&
              fabs(c.duration_s - 5.0) < 1e-9,
              "default (non-reuse) semantics changed: reuse=%d frames=%d dur=%g",
              c.reuse, c.frame_count, c.duration_s);
    }
    {
        char *v[] = { "p", "--present", "--reuse" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.reuse == 1 &&
              c.frame_count == TP_REUSE_DEFAULT_FRAMES &&
              fabs(c.duration_s - TP_REUSE_DEFAULT_DURATION) < 1e-9,
              "reuse defaults wrong: frames=%d dur=%g", c.frame_count, c.duration_s);
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--frame-count", "3" };
        CHECK(parse_rc(v, N(v), &c) == -1, "reuse --frame-count 3 accepted");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--frame-count", "1801" };
        CHECK(parse_rc(v, N(v), &c) == -1, "reuse --frame-count 1801 accepted");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--frame-count", "1800",
                      "--duration", "30" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.frame_count == 1800,
              "reuse max bounds rejected");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--frame-count", "4",
                      "--duration", "0.1" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.frame_count == 4,
              "reuse explicit small bounds rejected");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--duration", "0.05" };
        CHECK(parse_rc(v, N(v), &c) == -1, "reuse --duration 0.05 accepted");
    }
    {
        char *v[] = { "p", "--reuse" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--reuse without --present accepted");
    }
    {
        char *v[] = { "p", "--diagnostic-only", "--reuse" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--reuse with --diagnostic-only accepted");
    }

    /* --soak: explicit long-run ceiling raise; requires --present --reuse. */
    {
        char *v[] = { "p", "--present", "--reuse", "--soak" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.soak == 1 && c.reuse == 1 &&
              c.frame_count == TP_SOAK_DEFAULT_FRAMES &&
              fabs(c.duration_s - TP_SOAK_DEFAULT_DURATION) < 1e-9,
              "soak defaults wrong: frames=%d dur=%g", c.frame_count, c.duration_s);
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--soak",
                      "--duration", "120", "--frame-count", "7200" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.frame_count == 7200 &&
              fabs(c.duration_s - 120.0) < 1e-9,
              "soak 7200/120 rejected: frames=%d dur=%g", c.frame_count, c.duration_s);
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--soak",
                      "--duration", "1800", "--frame-count", "108000" };
        CHECK(parse_rc(v, N(v), &c) == 0 && c.frame_count == TP_SOAK_MAX_FRAMES,
              "soak max bounds rejected");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--soak",
                      "--frame-count", "108001" };
        CHECK(parse_rc(v, N(v), &c) == -1, "soak --frame-count 108001 accepted");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--soak",
                      "--duration", "1800.1" };
        CHECK(parse_rc(v, N(v), &c) == -1, "soak --duration 1800.1 accepted");
    }
    {
        /* Above the normal 1800 ceiling but allowed once --soak is explicit. */
        char *v[] = { "p", "--present", "--reuse", "--soak",
                      "--frame-count", "1801" };
        CHECK(parse_rc(v, N(v), &c) == 0, "soak --frame-count 1801 rejected");
    }
    {
        char *v[] = { "p", "--reuse", "--soak" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--soak without --present accepted");
    }
    {
        char *v[] = { "p", "--present", "--soak" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--soak without --reuse accepted");
    }
    {
        char *v[] = { "p", "--diagnostic-only", "--soak" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--soak with --diagnostic-only accepted");
    }
    {
        char *v[] = { "p", "--present", "--reuse", "--soak", "--verbose" };
        CHECK(parse_rc(v, N(v), &c) == -1, "--soak with --verbose accepted");
    }
    {
        /* Without --soak the normal reuse ceiling is unchanged (1801 rejected). */
        char *v[] = { "p", "--present", "--reuse", "--frame-count", "1801" };
        CHECK(parse_rc(v, N(v), &c) == -1, "normal reuse ceiling loosened by soak");
    }
}

/* Invalid CLI must be rejected strictly *before* any device call. */
static void test_cli_is_pre_device(void)
{
    tpstub_reset();
    tpstub_dc_reset();
    char *v[] = { "p", "--present", "--mode", "5x5", "--physical", "9x9" };
    int rc = tp_real_main(N(v), v);
    CHECK(rc == 2, "invalid CLI rc=%d expected 2", rc);
    CHECK(tpstub_dc_closes() == 0 && tpstub_gpu_destroys() == 0,
          "invalid CLI touched a device (dc_closes=%d gpu_destroys=%d)",
          tpstub_dc_closes(), tpstub_gpu_destroys());
}

/* ---------------------------------------------------------- lifecycle tests */

static void clear_env(void)
{
    unsetenv("TP_STUB_FAIL_STAGE");
    unsetenv("TP_STUB_RENDER_FAIL_IDX");
    unsetenv("TP_STUB_SUBMIT_LOST_IDX");
    unsetenv("TP_STUB_WAIT_RESULT");
    unsetenv("TP_STUB_FLIP_FAIL_IDX");
    unsetenv("TP_STUB_CANCEL_AFTER_FLIPS");
    unsetenv("TP_STUB_OPEN_PRESENT_FAIL");
    unsetenv("TP_STUB_ACQUIRE_FAIL");
    unsetenv("TP_STUB_STATUS_ENABLED");
    unsetenv("TP_STUB_STATUS_AFTER_UNBLANK");
    unsetenv("TP_STUB_VBLANK_ADVANCE");
    unsetenv("TP_STUB_FB_OPEN_FAIL");
    unsetenv("TP_STUB_UNBLANK_RESULT");
    unsetenv("TP_STUB_PRIOR_BLANK");
    unsetenv("TP_STUB_GET_STATUS_FAIL");
    unsetenv("TP_STUB_UNBLANK_PARTIAL");
    unsetenv("TP_STUB_RESTORE_RESULT");
    unsetenv("TP_STUB_RELEASE_MODE");
    unsetenv("TP_STUB_FB_BUDGET_FAIL");
}

static int order_before(const char *ev, const char *a, const char *b)
{
    const char *pa = strstr(ev, a);
    const char *pb = strstr(ev, b);
    return pa && pb && pa < pb;
}

static int run_case(const char *label, void (*setup)(void), int expect_rc,
                    int expect_safe, int expect_null, int expect_unsafe_quiesce)
{
    clear_env();
    tpstub_reset();
    tpstub_dc_reset();
    if (setup)
        setup();

    char *argv[] = { "probe", "--present", "--dc", "0", "--window", "0",
                     "--mode", "64x48", "--physical", "48x64",
                     "--orientation", "90", "--duration", "0.3",
                     "--frame-count", "2" };
    int rc = tp_real_main(N(argv), argv);
    const char *ev = tpstub_events();

    CHECK(order_before(ev, "dc_close", "gpu_destroy"),
          "%s: dc_close not before gpu_destroy (%s)", label, ev);
    CHECK(rc == expect_rc, "%s: rc=%d expected %d", label, rc, expect_rc);
    CHECK(tpstub_destroy_safe() == expect_safe,
          "%s: destroy safe=%d expected %d (%s)", label,
          tpstub_destroy_safe(), expect_safe, ev);
    CHECK(tpstub_destroy_null() == expect_null,
          "%s: destroy null=%d expected %d", label, tpstub_destroy_null(),
          expect_null);
    CHECK((strstr(ev, "gpu_quiesce_unsafe") != NULL) == expect_unsafe_quiesce,
          "%s: quiesce-unsafe flag mismatch (%s)", label, ev);
    return rc;
}

static void s_render0(void) { setenv("TP_STUB_RENDER_FAIL_IDX", "0", 1); }
static void s_render1(void) { setenv("TP_STUB_RENDER_FAIL_IDX", "1", 1); }
static void s_wait_timeout(void) { setenv("TP_STUB_WAIT_RESULT", "1", 1); }
static void s_wait_error(void) { setenv("TP_STUB_WAIT_RESULT", "-1", 1); }
static void s_submit_lost(void) { setenv("TP_STUB_SUBMIT_LOST_IDX", "0", 1); }
static void s_flip_fail(void) { setenv("TP_STUB_FLIP_FAIL_IDX", "0", 1); }
static void s_partial_scanout(void) { setenv("TP_STUB_FAIL_STAGE", "scanout", 1); }
static void s_null_init(void) { setenv("TP_STUB_FAIL_STAGE", "init", 1); }
static void s_cancel(void) { setenv("TP_STUB_CANCEL_AFTER_FLIPS", "1", 1); }

static void test_lifecycle(void)
{
    /* full success: all frames render/wait/flip, rc 0, safe destroy. */
    run_case("full-success", NULL, 0, 1, 0, 0);

    /* render failures (CPU-side): nonzero rc, no pending GPU -> safe destroy. */
    run_case("first-render-fail", s_render0, 5, 1, 0, 0);
    run_case("later-render-fail", s_render1, 5, 1, 0, 0);

    /* wait timeout/error: unsafe GPU -> quiesce refuses, destroys skipped. */
    run_case("wait-timeout", s_wait_timeout, 5, 0, 0, 1);
    run_case("wait-error", s_wait_error, 5, 0, 0, 1);

    /* vkQueueSubmit DEVICE_LOST also marks the GPU unsafe. */
    run_case("submit-device-lost", s_submit_lost, 5, 0, 0, 1);

    /* FLIP4 failure: nonzero rc, fence already confirmed -> safe destroy. */
    run_case("flip4-fail", s_flip_fail, 5, 1, 0, 0);

    /* partial init: probe bails mid-init; dc close + destroy still run. */
    run_case("partial-init-scanout", s_partial_scanout, 5, 1, 0, 0);

    /* null-device partial init: destroy sees a null device (clean instance). */
    run_case("null-device-init", s_null_init, 5, 1, 1, 0);

    /* cancel LAST: SIGINT sets the process-persistent stop flag. */
    run_case("cancel-after-1-flip", s_cancel, 5, 1, 0, 0);
}

/* ------------------------------------------------ head-enable lifecycle */

static void eh_argv(char **argv)
{
    argv[0] = "probe";
    argv[1] = "--present";
    argv[2] = "--dc";
    argv[3] = "0";
    argv[4] = "--window";
    argv[5] = "0";
    argv[6] = "--mode";
    argv[7] = "64x48";
    argv[8] = "--physical";
    argv[9] = "48x64";
    argv[10] = "--orientation";
    argv[11] = "90";
    argv[12] = "--duration";
    argv[13] = "0.3";
    argv[14] = "--frame-count";
    argv[15] = "2";
}

static void test_enable_head(void)
{
    char *argv[18];

    /* disabled head + no --enable-head => explicit refusal, no unblank/claim. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    eh_argv(argv);
    CHECK(tp_real_main(16, argv) == 5, "disabled+noflag: rc=%d expected 5", 5);
    CHECK(tpstub_unblanks() == 0, "disabled+noflag issued unblank");
    CHECK(tpstub_window_acquires() == 0, "disabled+noflag claimed a window");
    CHECK(tpstub_restores() == 0, "disabled+noflag restored a head it never changed");

    /* unblank ioctl failure => refuse. The ioctl may still have partially
     * changed the head, so head_changed is marked before the attempt and the
     * prior state is restored exactly once (after dc_close, before fb_close). */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    setenv("TP_STUB_UNBLANK_RESULT", "-1", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    {
        int rc = tp_real_main(17, argv);
        const char *ev = tpstub_events();
        CHECK(rc == 5, "unblank-fail: rc=%d expected 5", rc);
        CHECK(tpstub_unblanks() == 1, "unblank-fail: unblank not attempted");
        CHECK(tpstub_restores() == 1, "unblank-fail: restore count=%d expected 1", tpstub_restores());
        CHECK(tpstub_window_acquires() == 0, "unblank-fail claimed a window");
        CHECK(order_before(ev, "dc_close", "fb_restore"),
              "unblank-fail: restore not after dc_close (%s)", ev);
        CHECK(order_before(ev, "fb_restore", "fb_close"),
              "unblank-fail: fb_close not after restore (%s)", ev);
    }

    /* prior GET_STATUS failure => fail closed: never assume disabled, never
     * touch fb0 (no fb_open/unblank/restore), never claim a window. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_GET_STATUS_FAIL", "1", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    CHECK(tp_real_main(17, argv) == 5, "get-status-fail: rc!=5");
    CHECK(tpstub_unblanks() == 0, "get-status-fail: unblank issued");
    CHECK(tpstub_restores() == 0, "get-status-fail: restore issued");
    CHECK(tpstub_window_acquires() == 0, "get-status-fail: window claimed");

    /* partial unblank: ioctl errors yet the head may be partly enabled; the
     * prior state must still be restored exactly once and no window claimed. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    setenv("TP_STUB_UNBLANK_RESULT", "-1", 1);
    setenv("TP_STUB_UNBLANK_PARTIAL", "1", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    {
        int rc = tp_real_main(17, argv);
        const char *ev = tpstub_events();
        CHECK(rc == 5, "partial-unblank: rc=%d expected 5", rc);
        CHECK(tpstub_restores() == 1, "partial-unblank: restore count=%d expected 1", tpstub_restores());
        CHECK(tpstub_window_acquires() == 0, "partial-unblank claimed a window");
        CHECK(order_before(ev, "dc_close", "fb_restore"),
              "partial-unblank: restore not after dc_close (%s)", ev);
    }

    /* fb_restore error => nonzero result, but the DC fd is still closed before
     * the restore, fb0 is still closed, and GPU teardown still runs. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    setenv("TP_STUB_RESTORE_RESULT", "1", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    {
        int rc = tp_real_main(17, argv);
        const char *ev = tpstub_events();
        CHECK(rc == 5, "restore-fail: rc=%d expected 5", rc);
        CHECK(tpstub_restores() == 1, "restore-fail: restore count=%d expected 1", tpstub_restores());
        CHECK(order_before(ev, "dc_close", "fb_restore"),
              "restore-fail: restore not after dc_close (%s)", ev);
        CHECK(order_before(ev, "fb_restore", "fb_close"),
              "restore-fail: fb_close not after restore (%s)", ev);
        CHECK(strstr(ev, "gpu_destroy") != NULL,
              "restore-fail: gpu teardown did not run (%s)", ev);
        CHECK(tpstub_destroy_safe() == 1, "restore-fail: destroy not safe");
    }

    /* unblank returns 0 but status stays disabled => fail AND restore. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    setenv("TP_STUB_STATUS_AFTER_UNBLANK", "0", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    CHECK(tp_real_main(17, argv) == 5, "unblank-ok-status-disabled: rc!=5");
    CHECK(tpstub_restores() == 1, "unblank-ok-status-disabled: restore count=%d expected 1", tpstub_restores());
    CHECK(tpstub_window_acquires() == 0, "unblank-ok-status-disabled claimed a window");

    /* vblank stuck after a good unblank => fail and restore. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    setenv("TP_STUB_VBLANK_ADVANCE", "0", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    CHECK(tp_real_main(17, argv) == 5, "vblank-stuck: rc!=5");
    CHECK(tpstub_restores() == 1, "vblank-stuck: restore count=%d expected 1", tpstub_restores());

    /* enable-before-claim + restore-after-dc-close-before-gpu-destroy. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    {
        int rc = tp_real_main(17, argv);
        const char *ev = tpstub_events();
        CHECK(rc == 0, "enable-head full: rc=%d expected 0", rc);
        CHECK(order_before(ev, "fb_unblank", "window_acquire"),
              "unblank not before window acquire (%s)", ev);
        CHECK(order_before(ev, "dc_close", "fb_restore"),
              "fb_restore not after dc_close (%s)", ev);
        CHECK(order_before(ev, "fb_restore", "gpu_destroy"),
              "fb_restore not before gpu_destroy (%s)", ev);
    }
    CHECK(tpstub_restores() == 1, "enable-head full: restore count=%d expected 1", tpstub_restores());
    CHECK(tpstub_destroy_safe() == 1, "enable-head full: destroy not safe");

    /* original head already enabled => leave state alone (no unblank/restore). */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    eh_argv(argv);
    CHECK(tp_real_main(16, argv) == 0, "already-enabled: rc!=0");
    CHECK(tpstub_unblanks() == 0, "already-enabled issued unblank");
    CHECK(tpstub_restores() == 0, "already-enabled restored a head it did not change");
    CHECK(tpstub_window_acquires() == 1, "already-enabled did not claim a window");

    /* early failure: fb0 open fails => clean dc close, no restore. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    setenv("TP_STUB_FB_OPEN_FAIL", "1", 1);
    eh_argv(argv); argv[16] = "--enable-head";
    CHECK(tp_real_main(17, argv) == 5, "fb-open-fail: rc!=5");
    CHECK(tpstub_dc_closes() == 1, "fb-open-fail: dc not closed (%d)", tpstub_dc_closes());
    CHECK(tpstub_restores() == 0, "fb-open-fail restored");
    CHECK(tpstub_unblanks() == 0, "fb-open-fail unblanked");
    CHECK(tpstub_destroy_safe() == 1, "fb-open-fail: destroy not safe");

    /* diagnostics must never unblank or claim a window. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_STATUS_ENABLED", "0", 1);
    {
        char *dv[] = { "probe", "--diagnostic-only" };
        CHECK(tp_real_main(2, dv) == 0, "diagnostic-only: rc!=0");
    }
    CHECK(tpstub_unblanks() == 0, "diagnostic issued unblank");
    CHECK(tpstub_window_acquires() == 0, "diagnostic claimed a window");
    CHECK(tpstub_restores() == 0, "diagnostic restored");
}

/* ------------------------------------------------- reuse-mode lifecycle */

static void reuse_argv(char **argv)
{
    argv[0] = "probe";
    argv[1] = "--present";
    argv[2] = "--dc";
    argv[3] = "0";
    argv[4] = "--window";
    argv[5] = "0";
    argv[6] = "--mode";
    argv[7] = "64x48";
    argv[8] = "--physical";
    argv[9] = "48x64";
    argv[10] = "--orientation";
    argv[11] = "90";
    argv[12] = "--duration";
    argv[13] = "0.3";
    argv[14] = "--frame-count";
    argv[15] = "4";
    argv[16] = "--reuse";
}

static int count_substr(const char *hay, const char *needle)
{
    int n = 0;
    size_t nl = strlen(needle);
    const char *p = hay;
    while (nl && (p = strstr(p, needle)) != NULL) {
        n++;
        p += nl;
    }
    return n;
}

static void test_reuse_lifecycle(void)
{
    char *argv[17];

    /* Full success: 4 frames over 3 slots; frame 3 recycles slot 0 only after
     * release_retired; the recycle is the only frame that records an acquire. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    reuse_argv(argv);
    {
        int rc = tp_real_main(17, argv);
        const char *ev = tpstub_events();
        CHECK(rc == 0, "reuse full: rc=%d expected 0", rc);
        CHECK(tpstub_flips() == 4, "reuse full: flips=%d expected 4", tpstub_flips());
        CHECK(tpstub_releases() == 1, "reuse full: releases=%d expected 1",
              tpstub_releases());
        CHECK(strstr(ev, "release_retired") != NULL, "reuse full: no validated retirement");
        CHECK(strstr(ev, "gpu_render_ok_acquire") != NULL,
              "reuse full: recycled slot recorded no acquire");
        CHECK(tpstub_destroy_safe() == 1, "reuse full: destroy not safe");
        CHECK(order_before(ev, "dc_close", "gpu_destroy"), "reuse full: teardown order");
    }

    /* Delayed fence: the slot's own post fence is not retired in budget => stop
     * before overwriting it (3 flips only), no CPU fallback, clean teardown. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_RELEASE_MODE", "notready", 1);
    reuse_argv(argv);
    {
        int rc = tp_real_main(17, argv);
        const char *ev = tpstub_events();
        CHECK(rc == 5, "reuse notready: rc=%d expected 5", rc);
        CHECK(tpstub_flips() == 3, "reuse notready: flips=%d expected 3 (no overwrite)",
              tpstub_flips());
        CHECK(count_substr(ev, "gpu_render_ok") == 3,
              "reuse notready: renders=%d expected 3 (pending slot not rewritten)",
              count_substr(ev, "gpu_render_ok"));
        CHECK(strstr(ev, "release_notready") != NULL, "reuse notready: not observed");
        CHECK(tpstub_destroy_safe() == 1, "reuse notready: destroy not safe");
        CHECK(order_before(ev, "dc_close", "gpu_destroy"), "reuse notready: teardown order");
    }

    /* Unsupported ioctl (ENOTTY): fail closed, no recycle, no rewrite. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_RELEASE_MODE", "notty", 1);
    reuse_argv(argv);
    {
        const char *ev;
        CHECK(tp_real_main(17, argv) == 5, "reuse notty: rc!=5");
        ev = tpstub_events();
        CHECK(tpstub_flips() == 3, "reuse notty: flips=%d expected 3", tpstub_flips());
        CHECK(count_substr(ev, "gpu_render_ok") == 3,
              "reuse notty: renders=%d expected 3 (no rewrite)",
              count_substr(ev, "gpu_render_ok"));
    }

    /* Generic fence error verdict: same fail-closed stop. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_RELEASE_MODE", "fail", 1);
    reuse_argv(argv);
    {
        const char *ev;
        CHECK(tp_real_main(17, argv) == 5, "reuse fail: rc!=5");
        ev = tpstub_events();
        CHECK(tpstub_flips() == 3, "reuse fail: flips=%d expected 3", tpstub_flips());
        CHECK(count_substr(ev, "gpu_render_ok") == 3,
              "reuse fail: renders=%d expected 3 (no rewrite)",
              count_substr(ev, "gpu_render_ok"));
    }

    /* Render failure inside reuse: abort after partial progress, safe teardown. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_RENDER_FAIL_IDX", "0", 1);
    reuse_argv(argv);
    {
        int rc = tp_real_main(17, argv);
        CHECK(rc == 5, "reuse render-fail: rc=%d expected 5", rc);
        CHECK(tpstub_flips() == 0, "reuse render-fail: flips=%d expected 0", tpstub_flips());
        CHECK(tpstub_destroy_safe() == 1, "reuse render-fail: destroy not safe");
    }
}

/* --------------------------------------------- soak FB budget wiring */

static void test_soak_fb_budget(void)
{
    char *argv[18];

    /* --soak derives duration_s*1000+60000 and sets it once, before any FB
     * ioctl. reuse_argv is duration=0.3 => 300 + 60000 = 60300 ms. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    reuse_argv(argv);                 /* 0..16 (incl --reuse), argc 17 */
    argv[16] = "--soak";
    argv[17] = "--reuse";
    {
        int rc = tp_real_main(18, argv);
        CHECK(rc == 0, "soak budget: rc=%d expected 0", rc);
        CHECK(tpstub_fb_budget_calls() == 1, "soak budget: sets=%d expected 1",
              tpstub_fb_budget_calls());
        CHECK(tpstub_fb_budget_ms() == 60300u,
              "soak budget: value=%u expected 60300", tpstub_fb_budget_ms());
    }

    /* A normal (non-soak) reuse run must NOT raise the budget. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    reuse_argv(argv);
    (void)tp_real_main(17, argv);
    CHECK(tpstub_fb_budget_calls() == 0, "normal run raised the FB budget");

    /* Setter failure fails closed: no device touched, nonzero result. */
    clear_env(); tpstub_reset(); tpstub_dc_reset();
    setenv("TP_STUB_FB_BUDGET_FAIL", "1", 1);
    reuse_argv(argv);
    argv[16] = "--soak";
    argv[17] = "--reuse";
    {
        int rc = tp_real_main(18, argv);
        CHECK(rc == 5, "soak budget fail: rc=%d expected 5", rc);
        CHECK(tpstub_gpu_destroys() == 0,
              "soak budget fail: touched a device (destroys=%d)",
              tpstub_gpu_destroys());
        CHECK(tpstub_dc_closes() == 0,
              "soak budget fail: closed a dc it never opened");
    }
}

int main(void)
{
    /* Keep stderr (tp_log) quiet; only test results belong on stdout. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;

    test_cli();
    test_cli_is_pre_device();
    /* Run the head-enable cases before the lifecycle suite: the lifecycle
     * suite's cancel case raises SIGINT and sets the process-persistent stop
     * flag, which is never cleared. */
    test_enable_head();
    test_reuse_lifecycle();
    test_soak_fb_budget();
    test_lifecycle();

    printf("probe tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
