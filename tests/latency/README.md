# Screen-to-screen latency tests

The primary procedure is in [`docs/screen-to-screen-latency-test-plan.md`](../../docs/screen-to-screen-latency-test-plan.md).

For the current dual AR0234 image, use the frozen-image, optical measurement and
A/B procedure in [`docs/stereo-latency-baseline-plan.md`](../../docs/stereo-latency-baseline-plan.md).
The original single-camera instructions below remain a stimulus usage example.

Software-only stage profiling is opt-in with `TELEOP_METRICS=1`. See
[`docs/software-performance-baseline-2026-10-12.md`](../../docs/software-performance-baseline-2026-10-12.md)
for stage definitions and the incomplete hardware baseline. The native-only
`python3 tests/latency/test_software_metrics.py` runs real dual-stream UDP/VAAPI
tests using `build/host-metrics/bin/host-viewer` (override `TELEOP_TEST_VIEWER`).
It checks legacy parsing, marked complete AUs, per-stream mixed compatibility,
forced lookahead and rejection of an incomplete AU (all-IDR recovery sample).
Stage times are process-local; VAAPI frame availability does not prove that GPU
work or panel scanout has completed.

`tools/test-frame-boundary-wait --output logs/frame-boundary/new-run` is a native
VAAPI boundary experiment. It links the production parser/decoder and compares
normal byte-stream parsing, explicitly complete-AU parsing and direct complete-AU
submission on a generated multi-slice stream. Repeat `--input path.h264` to also
check recordings (no B-frame reordering). The offset oracle is offline ffprobe;
this does not change live viewer defaults or license treating slices as frames.
See [`docs/frame-boundary-wait-exploration-2026-10-13.md`](../../docs/frame-boundary-wait-exploration-2026-10-13.md).

The first optical stimulus is built on the CachyOS host only:

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/build-host

# Example for a portrait/rotated 144 Hz auxiliary display.  Put the source
# above the receiver; replace the geometry with the display's desktop pixels.
./build/host/bin/latency-stimulus \
  --width 1080 --height 900 --x 1920 --y 0 --borderless --vsync on

./build/host/bin/host-viewer \
  --port 5000 --frames 0 --timeout-ms 60000 \
  --window-width 1080 --window-height 900 \
  --window-x 1920 --window-y 900 --borderless
```

The Aura camera must only see the upper `latency-stimulus` window. Use an external
240 fps-or-faster camera to record both vertically stacked windows and subtract the
decoded lower time code from the upper one. The stimulus prints its selected SDL
renderer, actual VSync setting, and present rate once per second; retain that log
with each measurement.

KDE Wayland intentionally does not allow normal applications to choose their exact
top-level position. On this desktop `--x`/`--y` are best-effort requests: use the
window titles **Aura optical latency stimulus** and **Luckfox Aura Teleop** with KWin's
manual/quick-tiling controls to put the stimulus above the viewer. The supplied sizes
remain useful even when KWin owns the final position.
