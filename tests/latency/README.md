# Screen-to-screen latency tests

The primary procedure is in [`docs/screen-to-screen-latency-test-plan.md`](../../docs/screen-to-screen-latency-test-plan.md).

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
