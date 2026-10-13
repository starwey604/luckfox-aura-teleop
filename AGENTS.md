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

There is no CI; all build/test steps are run manually. The origin remote is GitHub.

## Commands

```sh
./tools/select-teleop-board <sdk> [profile]  # ar0234-stereo-debug is diagnostic only; symlink .BoardConfig.mk to the teleop BoardConfig
./tools/sdk-patch {check|status|apply|export} <sdk>   # patch stack management
./tools/verify-sdk-baseline <sdk>     # all 63 locked vendor revisions + saved patch prefixes
./tools/test-sdk-replay <sdk>         # isolated clean-baseline clones, compare patched trees
./tools/build-aura                     # cross-compile sender (in Distrobox)
./tools/check-aura-runtime             # verify AArch64 ELF, RPATH, SDK staging + images
./tools/deploy-aura                    # scp sender to /userdata, print --help on board
./tools/build-host                     # host viewer + usb camera sender + synthetic sender
./tools/test-transport-loopback        # build, run ctest protocol suite, host UDP round-trip
./tools/test-vaapi-decoder             # generate H.264 via ffmpeg, prove VAAPI hw decode
./tools/test-vulkan-presenter          # VAAPI -> DRM PRIME -> Vulkan/SDL3 window
./tools/test-aura-software-baseline --camera {0|1|dual} --output <new-dir>  # bounded SSH/native profiling
./tools/test-usb-camera-loopback       # /dev/video0 -> UDP -> VAAPI/Vulkan (needs real camera)
./tools/flash-aura-emmc [--dry-run]    # full eMMC flash via Rockchip upgrade_tool
```

Order matters: apply the SDK patch stack and `select-teleop-board` before SDK builds;
`./build.sh media` before `./tools/build-aura`.

## Patch stack

- `sdk/series.tsv` (pipe-delimited `repo-path|baseline-commit|branch`) is authoritative;
  the `series.toml` name in `docs/` is stale. Patch dirs mirror SDK repo paths.
- `tools/sdk-patch` requires a real `.repo` checkout. `check` replays the series on a temporary
  index; `apply` preflights all projects, accepts the baseline or an exact saved patch prefix,
  applies only missing patches with `git am --3way`, and never runs `reset`/`clean`.
  Other vendor SDK versions need a separate matching checkout or an explicit port.
  The fifth patch target is `sysdrv/drv_ko/kmpp/kmpp` (`teleop/kmpp-irq-locks`):
  object-definition get/put lists are also accessed by slice IRQ callbacks, so their
  paired locks must save/restore local IRQ state. Preserve the opaque OSAL layouts.
  Meta-server and allocation-accounting lists have the same IRQ-sharing requirement;
  pool diagnostics must snapshot under IRQ-safe locking and format after unlocking.
  `tests/aura/test_kmpp_pool_snapshot.py` extracts the real SDK snapshot function for
  ASan/UBSan host testing in Distrobox; it does not simulate hardware IRQ execution.
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
- The user requests short camera tests while the board has no fan: default 120 frames
  per camera, no automatic long runs. The profiling tool rejects >300 frames without
  `--allow-long` and a start temperature >=70 C by default (user raised the limit on 2026-10-12). Do not use `--allow-long`
  unless the user later explicitly requests longer tests. Repeated stereo restart
  can lock the board; a manual user reboot is required after loss of connectivity.

## Camera notes

- Product name ISG1321 == kernel driver/`MIA1321`; I²C addr `0x60`, 26 MHz xvclk.
- The sensor registers three 1280×1080 modes (RAW12 30 fps, RAW10 60 fps, RAW10 ~117 fps); there
  is no native 720p mode. A `mia1321_linear_10_1280x720_regs` table actually registers 1280×1080@30.
- For high-fps bring-up verify `dmesg` shows `dphy0, data_rate_mbps 1728`, then run a 120-frame
  `v4l2-ctl -d /dev/video13 --stream-mmap=3 --stream-count=120 --stream-to=/dev/null --verbose`.
  MIPI `ERR2`/ECC/CRC errors are usually the FPC/CSI connector, not software — reseat first.

## Tests

- `tests/sdk/test_patch_stack.py` checks sequential patches, partial/repeated apply, refusal
  before mutation, manifest mismatches and relative BoardConfig selection. Run in Distrobox.
- `tests/aura/` exercises the production sender with a stateful fake MPI backend in Distrobox
  (`TELEOP_BUILD_MPI_LIFECYCLE_TESTS=ON`, SDK headers required). `tools/test-aura-lifecycle`
  runs bounded real-board restart, fault-injection, process-lock and signal tests.
  Stereo uses one `aura-sender --dual` process/SYS session; do not launch two MPI sender processes.
- For observer-effect controls, `test-aura-software-baseline --minimal` disables live
  kernel/thread sampling and stage metrics and starts the sender directly. Netconsole
  capture defaults to console level 1 (emergency only); do not automatically re-enable
  verbose level 8 after its SYS_Exit lockup reproduction. `AURA_FAIL_AFTER=system.initialized`
  probes SYS_Init/WaitFreeMB/Exit without cameras; exit 1 is expected for that injection.
- `tests/protocol/` has the real unit test (`ctest` via `test-transport-loopback`).
- `tests/latency/test_software_metrics.py` is a native-only dual UDP/VAAPI timing
  association test. Use `TELEOP_METRICS=1` for opt-in process-local stage histograms;
  histograms belong to each application thread; each sender worker prints its own stream prefix.
  `tests/h264/` and `tests/usb-camera/` document
  host-only hardware validation; see their READMEs for acceptance output
  (`hardware_frames == frames`, `presenter: frames=...`, zero malformed/dropped).
- Keep internal software timestamps separate from screen-to-screen numbers; only `--same-clock`
  loopback gives a valid sender→receiver delay without external optical measurement.
- `tools/test-frame-boundary-wait --output <new-dir>` is native-only: it uses the real
  VAAPI decoder with ffprobe-provided complete AU boundaries to measure parser lookahead.
  Mode3 packs still report bFrameEnd on every slice; never mark such slices as complete AUs.
  Sender now defaults to `--output-mode frame` (SDK mode1): one nonempty whole-frame pack
  is sent immediately with CompleteAccessUnit flag bit1. `--output-mode slice` retains
  next-PTS grouping and marks only the assembled AU. Viewer directly submits marked
  AUs, retains the parser for unmarked legacy chunks/files, and rejects per-stream
  contract changes. `--force-stream-parser` is an A/B diagnostic only.
