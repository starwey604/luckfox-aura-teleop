#!/usr/bin/env python3
"""Check the real driver sequencer against the vendor INI with mocked I/O.

This does not emulate the sensor: it verifies emitted writes/delays, default
mode timing, stream state and error cleanup without touching an I2C device.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("AURA_SDK_ROOT", ROOT.parent / "aura-sdk")).resolve()
DRIVER = SDK / "sysdrv/source/kernel/drivers/media/i2c/ar0234.c"
HEADER = DRIVER.with_name("ar0234-decxin-3478v1.h")
INI = ROOT / "references/3717878140AR0234_24M_MIPI_2lane_1080p_60fps_45Mhz_10bit(1)(2)(2)(1).ini"
BUILD = ROOT / "build/ar0234-logic-check"


def function(source, name):
    match = re.search(r"^static [^\n]*\b" + name + r"\(", source, re.M)
    if not match:
        raise ValueError(f"driver function missing: {name}")
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def declaration(source, pattern):
    match = re.search(pattern + r".*?\n};", source, re.S)
    if not match:
        raise ValueError(f"driver declaration missing: {pattern}")
    return match[0]


def vendor_events():
    section = INI.read_text().split("[720P 99.2fps]", 1)[1]
    events = []
    for line in section.splitlines():
        write = re.match(r"(0x[\da-fA-F]+)\s*,\s*(0x[\da-fA-F]+)", line)
        delay = re.match(r"DELAY\s*=\s*(\d+)", line)
        if write:
            events.append(("W", int(write[1], 16), int(write[2], 16)))
        elif delay:
            events.append(("D", int(delay[1])))
    return events


def build_harness():
    source = DRIVER.read_text()
    definitions = "\n".join(re.findall(r"^#define AR0234_.*$", source, re.M))
    declarations = "\n".join([
        declaration(source, r"struct ar0234_regval \{"),
        '#include "' + str(HEADER) + '"',
        declaration(source, r"struct ar0234_reg_sequence \{"),
        declaration(source, r"struct ar0234_mode \{"),
        *[declaration(source, rf"static const struct ar0234_regval {name}\[\] = \{{")
          for name in ("ar0234_1920x1200_config", "ar0234_1080p_config",
                       "ar0234_720p_config", "ar0234_960x600_config")],
        declaration(source, r"static const struct ar0234_mode ar0234_modes\[\] = \{"),
        declaration(source, r"static const struct ar0234_mode ar0234_decxin_modes\[\] = \{"),
        declaration(source, r"static const u32 ar0234_freq_pixclk\[\] = \{"),
    ])
    preamble = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
enum { AR0234_LANE_COUNT_ID_2LANE, AR0234_LANE_COUNT_ID_4LANE };
struct v4l2_rect { int left, top; unsigned int width, height; };
struct v4l2_fract { u32 numerator, denominator; };
'''
    mocks = r'''
struct ar0234;
struct handler { struct ar0234 *ar; };
struct ar0234 {
    bool decxin_profile, streaming;
    const struct ar0234_mode *cur_mode;
    const struct ar0234_mode *modes;
    unsigned int num_modes;
    int lane_count_id, mutex, pm_refs;
    void *dev, *hflip, *vflip;
    struct handler ctrl_handler;
    u16 mfr_30ba;
};
struct v4l2_subdev { struct ar0234 *ar; };
struct v4l2_mbus_framefmt { u32 width, height; };
struct v4l2_subdev_state { struct v4l2_mbus_framefmt fmt; };
struct v4l2_subdev_selection { u32 pad, which, target; struct v4l2_rect r; };
enum { V4L2_SUBDEV_FORMAT_ACTIVE, V4L2_SUBDEV_FORMAT_TRY };
enum { V4L2_SEL_TGT_CROP, V4L2_SEL_TGT_NATIVE_SIZE,
       V4L2_SEL_TGT_CROP_DEFAULT, V4L2_SEL_TGT_CROP_BOUNDS };
static const struct ar0234_mode *nearest(const struct ar0234_mode *modes,
    unsigned int count, u32 width, u32 height) {
    for (unsigned int i = 0; i < count; i++)
        if (modes[i].width == width && modes[i].height == height) return &modes[i];
    return &modes[0];
}
#define v4l2_find_nearest_size(modes, count, wf, hf, w, h) nearest(modes, count, w, h)
static struct v4l2_mbus_framefmt *v4l2_subdev_get_try_format(
    struct v4l2_subdev *sd, struct v4l2_subdev_state *state, int pad) {
    (void)sd; (void)pad; return &state->fmt;
}
static unsigned int fail_at, write_count;
static bool control_failure;
static bool id_failure, pm_failure;
static struct ar0234 *to_ar0234(struct v4l2_subdev *sd) { return sd->ar; }
static void mutex_lock(int *m) { (void)m; }
static void mutex_unlock(int *m) { (void)m; }
static void msleep(unsigned int ms) { printf("D %u\n", ms); }
static int ar0234_write_reg(struct ar0234 *ar, u32 reg, u16 value) {
    (void)ar;
    printf("W %u %u\n", reg & 0xffff, value);
    return ++write_count == fail_at ? -EIO : 0;
}
static int ar0234_read_reg(struct ar0234 *ar, u32 reg, u16 *id) {
    (void)ar;
    printf("R %u\n", reg & 0xffff);
    *id = 0x0a56;
    return id_failure ? -EIO : 0;
}
static int __v4l2_ctrl_handler_setup(struct handler *handler) {
    (void)handler;
    puts("C");
    return control_failure ? -EIO : 0;
}
static void __v4l2_ctrl_grab(void *ctrl, bool on) { (void)ctrl; printf("G %u\n", on); }
static int pm_runtime_resume_and_get(void *dev) {
    struct ar0234 *ar = dev;
    if (pm_failure) { puts("PM_FAIL"); return -EIO; }
    ar->pm_refs++;
    puts("PM+");
    return 0;
}
static void pm_runtime_put(void *dev) {
    struct ar0234 *ar = dev;
    ar->pm_refs--;
    puts("PM-");
}
'''
    functions = "\n".join(function(source, name) for name in [
        "ar0234_write_array", "ar0234_write_sequence", "ar0234_hts",
        "ar0234_default_vblank", "ar0234_pixel_rate", "ar0234_frame_interval",
        "ar0234_set_stream_state", "ar0234_start_decxin_stream", "ar0234_get_selection",
    ])
    # Select the vendor branch; generic source initialization is a separate profile.
    vendor_dispatch = "static int ar0234_start_stream(struct ar0234 *ar) { return ar0234_start_decxin_stream(ar); }\n"
    stream = function(source, "ar0234_s_stream") + "\n" + function(source, "ar0234_read_chip_id")
    main = r'''
int main(int argc, char **argv) {
    struct ar0234 ar = { .decxin_profile = true, .cur_mode = &ar0234_decxin_modes[0],
                        .lane_count_id = AR0234_LANE_COUNT_ID_2LANE };
    struct v4l2_subdev sd = { .ar = &ar };
    struct v4l2_fract interval;
    if (argc > 3 && strcmp(argv[3], "chip") && strcmp(argv[3], "selection")) {
        unsigned int mode = (unsigned int)strtoul(argv[3], NULL, 10);
        if (mode >= ARRAY_SIZE(ar0234_modes)) return 2;
        ar.decxin_profile = false;
        ar.cur_mode = &ar0234_modes[mode];
    }
    ar.dev = &ar;
    ar.ctrl_handler.ar = &ar;
    fail_at = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 0;
    control_failure = argc > 2 ? atoi(argv[2]) : false;
    if (argc > 3 && !strcmp(argv[3], "selection")) {
        for (unsigned int i = 0; i < ARRAY_SIZE(ar0234_modes) + 1; i++) {
            ar.cur_mode = i ? &ar0234_modes[i - 1] : &ar0234_decxin_modes[0];
            struct v4l2_subdev_selection sel = {
                .which = V4L2_SUBDEV_FORMAT_ACTIVE, .target = V4L2_SEL_TGT_CROP_BOUNDS
            };
            int ret = ar0234_get_selection(&sd, NULL, &sel);
            printf("BOUNDS %d %d %d %u %u\n", ret, sel.r.left, sel.r.top, sel.r.width, sel.r.height);
            sel.target = V4L2_SEL_TGT_CROP;
            ret = ar0234_get_selection(&sd, NULL, &sel);
            printf("CROP %d %d %d %u %u\n", ret, sel.r.left, sel.r.top, sel.r.width, sel.r.height);
        }
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "chip")) {
        u16 id = 0;
        id_failure = argc > 4 ? atoi(argv[4]) : false;
        pm_failure = argc > 5 ? atoi(argv[5]) : false;
        int ret = ar0234_read_chip_id(&ar, &id);
        printf("IDRESULT %d %u %d\n", ret, id, ar.pm_refs);
        return 0;
    }
    ar0234_frame_interval(&ar, ar.cur_mode, ar0234_default_vblank(ar.cur_mode), &interval);
    printf("MODE %zu %u %u %u %u %u %u %u\n",
           ar.decxin_profile ? ARRAY_SIZE(ar0234_decxin_modes) : ARRAY_SIZE(ar0234_modes),
           ar.cur_mode->width, ar.cur_mode->height, ar.cur_mode->line_length,
           ar.cur_mode->vts_def, ar.cur_mode->exposure_def,
           interval.numerator, interval.denominator);
    if (!ar.decxin_profile) return 0;
    int ret = ar0234_s_stream(&sd, 1);
    printf("RESULT %d %d %d\n", ret, ar.streaming, ar.pm_refs);
    if (!ret) {
        ret = ar0234_s_stream(&sd, 1);
        printf("REPEAT %d %d %d\n", ret, ar.streaming, ar.pm_refs);
        ret = ar0234_s_stream(&sd, 0);
        printf("STOP %d %d %d\n", ret, ar.streaming, ar.pm_refs);
    }
    return 0;
}
'''
    BUILD.mkdir(parents=True, exist_ok=True)
    harness = BUILD / "profile_harness.c"
    harness.write_text("\n".join([preamble, definitions, declarations, mocks, functions, vendor_dispatch, stream, main]))
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(BUILD / "profile_harness")], check=True)


class VendorProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_harness()

    def run_profile(self, fail_at=0, control_failure=0):
        output = subprocess.check_output([str(BUILD / "profile_harness"), str(fail_at), str(control_failure)], text=True)
        return [tuple([parts[0]] + [int(x) for x in parts[1:]]) for parts in map(str.split, output.splitlines())]

    def test_defaults_and_exact_vendor_sequence(self):
        events = self.run_profile()
        self.assertEqual(events[0], ("MODE", 1, 1280, 720, 612, 736, 514, 1813968, 180000000))
        self.assertEqual(events[1:3], [("PM+",), ("W", 0x3064, 0x1802)])
        oracle = vendor_events()
        self.assertEqual(len([x for x in oracle if x[0] == "W"]), 161)
        self.assertEqual(sum(x[1] for x in oracle if x[0] == "D"), 920)
        self.assertEqual(events[3:3 + len(oracle)], oracle)
        self.assertEqual(events[3 + len(oracle):], [
            ("C",), ("G", 1), ("G", 1), ("RESULT", 0, 1, 1), ("REPEAT", 0, 1, 1),
            ("W", 0x301a, 0x2058), ("PM-",), ("G", 0), ("G", 0), ("STOP", 0, 0, 0),
        ])

    def test_driver_chip_id_read_balances_runtime_power(self):
        for id_failure, pm_failure, expected in [
            (0, 0, "PM+\nR 12288\nPM-\nIDRESULT 0 2646 0\n"),
            (1, 0, "PM+\nR 12288\nPM-\nIDRESULT -5 2646 0\n"),
            (0, 1, "PM_FAIL\nIDRESULT -5 0 0\n"),
        ]:
            with self.subTest(id_failure=id_failure, pm_failure=pm_failure):
                output = subprocess.check_output(
                    [str(BUILD / "profile_harness"), "0", "0", "chip", str(id_failure), str(pm_failure)],
                    text=True)
                self.assertEqual(output, expected)

    def test_reference_mode_timings_remain_available(self):
        for index, (width, height) in enumerate(((1920, 1200), (1920, 1080),
                                                (1280, 720), (960, 600))):
            with self.subTest(index=index):
                output = subprocess.check_output(
                    [str(BUILD / "profile_harness"), "0", "0", str(index)], text=True)
                values = tuple(int(x) for x in output.split()[1:])
                self.assertEqual(values, (4, width, height, 612, height + 16, 100,
                                          2448 * (height + 21), 180000000))

    def test_cif_bounds_use_output_coordinates_without_changing_native_crop(self):
        output = subprocess.check_output(
            [str(BUILD / "profile_harness"), "0", "0", "selection"], text=True)
        rows = [line.split() for line in output.splitlines()]
        sizes = ((1280, 720), (1920, 1200), (1920, 1080), (1280, 720), (960, 600))
        for index, (width, height) in enumerate(sizes):
            with self.subTest(mode=index):
                self.assertEqual(rows[index * 2], ["BOUNDS", "0", "0", "0", str(width), str(height)])
                crop = rows[index * 2 + 1]
                self.assertEqual(crop[0:2], ["CROP", "0"])
                native_width, native_height = (1920, 1200) if index == 4 else (width, height)
                self.assertEqual(crop[4:], [str(native_width), str(native_height)])
                self.assertGreaterEqual(int(crop[2]), 8)
                self.assertGreaterEqual(int(crop[3]), 8)

    def test_i2c_failure_aborts_and_releases_power(self):
        # Cover metadata, initial standby, calibration and final stream-on writes.
        expected = [("W", 0x3064, 0x1802)] + vendor_events()
        for fail_at in (1, 2, 80, 162):
            with self.subTest(fail_at=fail_at):
                events = self.run_profile(fail_at=fail_at)
                consumed = []
                writes = 0
                for event in expected:
                    consumed.append(event)
                    writes += event[0] == "W"
                    if writes == fail_at:
                        break
                self.assertEqual(events[2:2 + len(consumed)], consumed)
                self.assertEqual(events[2 + len(consumed):], [
                    ("W", 0x301a, 0x2058), ("PM-",), ("RESULT", -5, 0, 0),
                ])

    def test_control_setup_failure_stops_sensor(self):
        events = self.run_profile(control_failure=1)
        self.assertEqual(events[-4:], [
            ("C",), ("W", 0x301a, 0x2058), ("PM-",), ("RESULT", -5, 0, 0),
        ])


if __name__ == "__main__":
    unittest.main(verbosity=2)
