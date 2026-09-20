# AGENTS.md

Workspace for Luckfox Aura (RV1126B) low-latency stereo video teleop. This repo is
not a standalone app: it holds the C++ sender/viewer, the SDK patch stack, and the
build/deploy/test scripts. The vendor SDK checkout lives in the adjacent `../aura-sdk`
(Android `repo` tree, not a single git repo) and is managed via patches, not committed here.

Docs in `docs/` are Chinese planning/records; treat config and scripts as the source of truth.

## Two build environments (hard requirement)

- **Target (`aura-sender`)** — cross-compiled in the Ubuntu 22.04 `aura-sdk` Distrobox,
  using the SDK's `aarch64-rockchip1240-linux-gnu` toolchain and `/oem/usr/lib` RUNPATH.
  `./tools/build-aura` defaults to SDK `../aura-sdk` and build dir `build/aura`
  (override with `AURA_SDK_ROOT`). The SDK media libs must be built first:
  `cd ../aura-sdk && ./build.sh media`.
- **Host (`host-viewer`, `usb-camera-sender`)** — built and run on the CachyOS host only.
  Their CMake scripts hard-fail if `/run/.containerenv` or `/.dockerenv` exists, so never
  configure them inside Distrobox. `./tools/build-host` also builds `transport-test-sender`.

There is no CI and no git remote; all build/test steps are run manually.

## Commands

```sh
./tools/select-teleop-board <sdk>      # symlink .BoardConfig.mk to the teleop BoardConfig
./tools/sdk-patch {check|status|apply|export} <sdk>   # patch stack management
./tools/build-aura                     # cross-compile sender (in Distrobox)
./tools/check-aura-runtime             # verify AArch64 ELF, RPATH, SDK staging + images
./tools/deploy-aura                    # scp sender to /userdata, print --help on board
./tools/build-host                     # host viewer + usb camera sender + synthetic sender
./tools/test-transport-loopback        # build, run ctest protocol suite, host UDP round-trip
./tools/test-vaapi-decoder             # generate H.264 via ffmpeg, prove VAAPI hw decode
./tools/test-vulkan-presenter          # VAAPI -> DRM PRIME -> Vulkan/SDL3 window
./tools/test-usb-camera-loopback       # /dev/video0 -> UDP -> VAAPI/Vulkan (needs real camera)
./tools/flash-aura-emmc [--dry-run]    # full eMMC flash via Rockchip upgrade_tool
```

Order matters: apply the SDK patch stack and `select-teleop-board` before SDK builds;
`./build.sh media` before `./tools/build-aura`.

## Patch stack

- `sdk/series.tsv` (pipe-delimited `repo-path|baseline-commit|branch`) is authoritative;
  the `series.toml` name in `docs/` is stale. Patch dirs mirror SDK repo paths.
- `tools/sdk-patch` requires a real `.repo` checkout. `apply` refuses dirty worktrees or a
  HEAD that is not the pinned baseline, uses `git am --3way`, and never runs `reset`/`clean`.
  Only `export` pulls from the listed `teleop/*` branches; don't commit one-off SDK experiments.
- After changing SDK config, prefer editing the source locations under `sysdrv/tools/...` /
  the BoardConfig patch. Do not edit `sysdrv/source/buildroot/buildroot-2025.02.6`, which is
  deleted and re-extracted by `buildroot_create`.
- Teleop Buildroot intentionally uses GCC 12, not the vendor GCC 13 defconfig, to keep
  `v4l-utils` (`v4l2-ctl`, `media-ctl`).

## Source layout gotchas

- `aura-sender`'s CMake builds only `src/aura-sender/src/mpp_sender.cpp` (the real VI→VENC
  hardware path). `src/aura-sender/src/main.cpp` is the synthetic UDP source and is compiled
  only by `transport-test-sender`. Editing `main.cpp` does not change `aura-sender`.
- All targets are C++20 with `-Wall -Wextra -Wpedantic -Wconversion`; keep code warning-clean.
- `host-viewer` accepts only VAAPI hardware decode (default `/dev/dri/renderD128`) and refuses
  software fallback; it needs ffmpeg, SDL3, Vulkan, and libplacebo dev packages on the host.

## Board / deployment

- Direct-link networking is pinned: board `eth0=192.168.1.10/24`, CachyOS host `192.168.1.2/24`,
  SSH as `root`. Test destination defaults to `192.168.1.2:5000`.
- `/userdata` is the persistent eMMC partition for frequently replaced test binaries; the
  sender's `/oem/usr/lib` RUNPATH means no `LD_LIBRARY_PATH` is needed.
- The growable `-(rootfs)` partition breaks vendor `rkdownload.sh` (`Size is invalid`, rootfs
  silently skipped). Use `tools/flash-aura-emmc`, or for rootfs-only updates after a Loader-mode
  reset: `sudo ./upgrade_tool wl 0x607C40 ../../../output/image/rootfs.img && sudo ./upgrade_tool rd`
  (the `0x607C40` offset is tied to the current teleop `env.img`; don't reuse across layouts).
  `flash-aura-emmc` overwrites `userdata`.

## Camera notes

- Product name ISG1321 == kernel driver/`MIA1321`; I²C addr `0x60`, 26 MHz xvclk.
- The sensor registers three 1280×1080 modes (RAW12 30 fps, RAW10 60 fps, RAW10 ~117 fps); there
  is no native 720p mode. A `mia1321_linear_10_1280x720_regs` table actually registers 1280×1080@30.
- For high-fps bring-up verify `dmesg` shows `dphy0, data_rate_mbps 1728`, then run a 120-frame
  `v4l2-ctl -d /dev/video13 --stream-mmap=3 --stream-count=120 --stream-to=/dev/null --verbose`.
  MIPI `ERR2`/ECC/CRC errors are usually the FPC/CSI connector, not software — reseat first.

## Tests

- `tests/protocol/` has the real unit test (`ctest` via `test-transport-loopback`).
- `tests/latency/` is still a placeholder. `tests/h264/` and `tests/usb-camera/` document
  host-only hardware validation; see their READMEs for acceptance output
  (`hardware_frames == frames`, `presenter: frames=...`, zero malformed/dropped).
- Keep internal software timestamps separate from screen-to-screen numbers; only `--same-clock`
  loopback gives a valid sender→receiver delay without external optical measurement.
