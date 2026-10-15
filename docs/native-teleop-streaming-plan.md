# Aura 原生低延迟传输程序开发与集成方案

## 1. 目标与结论

本项目在 `luckfox-aura-teleop` 中维护一套原生的低延迟视频程序：

- `aura-sender`：运行在 Luckfox Aura（RV1126B）上，采集 ISG1321/MIA1321，使用 Rockchip MPP/RK MPI 硬件编码并通过 UDP 发送；
- `host-viewer`：运行在 CachyOS x86_64 主机上，接收 UDP 码流、解码并显示；
- `common`：保存网络协议、统计结构、时间戳和公共测试代码。

正式性能路径不依赖完整 GStreamer 管线。GStreamer 仍作为摄像头、码流和网络功能的参考实现，用于交叉验证和排障。

原生程序的主要收益是完全控制缓冲数量、丢帧策略、slice 输出、UDP 分包和显示时机。优化良好的 GStreamer 封装本身通常只增加少量延迟，因此原生实现的价值主要是消除不透明的队列、用户态复制和时钟同步，而不是单纯更换框架。

720p60 H.264 的阶段目标为：

| 阶段 | P50 screen-to-screen | P95 screen-to-screen |
|---|---:|---:|
| 首个稳定原生闭环 | 40～65 ms | 55～85 ms |
| wrap、slice 和显示调优后 | 30～50 ms | 40～65 ms |
| 激进优化目标 | 25～40 ms | 35～50 ms |

## 2. 总体数据通路

```text
Aura sender
ISG1321/MIA1321
  → VICAP/VI
  → ISP/VPSS（只在格式转换、裁剪或缩放时使用）
  → RK MPI VENC / MPP H.264
  → slice packet
  → UDP

CachyOS viewer
UDP
  → 按 stream_id 独立分包校验与帧重组
  → FFmpeg libavcodec（软件或 VAAPI）
  → Vulkan/DRM/SDL 显示
```

`host-viewer --dual-stream` 接收 `stream_id` 为 0 和 1 的两路码流。两路各自使用
UDP 重组器、Annex-B parser 和 VAAPI decoder；显示器只保留每路最新帧，并将 0 放在窗口
上半区、1 放在下半区。这样一侧编码或网络片段延迟不会污染另一侧的 H.264 parser 状态。

当前 Aura stereo 镜像的程序预设为：`--camera 0` 对应 `m00_b_mia1321`、`rkisp-vir0`、
`VI 0/0/0`、`VENC 0`、`stream_id 0`；`--camera 1` 对应 `m01_b_mia1321`、`rkisp-vir1`、
`VI 1/1/0`、`VENC 1`、`stream_id 1`。双路使用一个 `sender --dual` 进程，统一 SYS 初始化和退出；
无需在命令行重复填写 VI/VENC 编号。sender 默认编码基线为 1280×1080、100 fps、30 Mbit/s
CBR、GOP 15、wrap 256 行、slice 120。

当前 AR0234 双摄用 `/userdata/run-ar0234-stereo` 应用曝光控制并传入 1280×720、wrap=0。
2026-10-15起，#19 fastreadout对应launcher默认VBLANK506、FPS120（实际约119.95fps）、
exposure735（4.998ms）、模拟/数字增益48/512，码率仍30Mbps/路。
用户确认画质后，AR0234 launcher默认编码调整为GOP15、CAVLC、4×4变换、不拆slice；
正常sender已更新，直接运行launcher即可。[当前固化配置](codec-defaults-2026-10-15.md)。
可通过环境变量覆盖；[当前默认与编解码调优计划](codec-low-latency-tuning-plan-2026-10-15.md)。
两路配置完成后统一绑定，工作线程全部退出并释放码流后才销毁资源和 SYS_Exit。
详见 [生命周期排查](sender-lifecycle-debug-2026-10-12.md)。

发送端优先复用 SDK 已有的 RK MPI sample、VENC 配置和内存管理代码。VI、VPSS 和 VENC 尽可能通过 `RK_MPI_SYS_Bind()` 连接，使图像留在 DMA buffer 中。应用只获取编码后的 packet，不把原始帧复制到用户态。

## 3. 推荐仓库结构

```text
luckfox-aura-teleop/
├── src/
│   ├── common/
│   │   ├── include/teleop/*.hpp
│   │   ├── protocol.cpp
│   │   ├── reassembler.cpp
│   │   └── udp_socket.cpp
│   ├── aura-sender/
│   │   ├── CMakeLists.txt
│   │   └── src/
│   └── host-viewer/
│       ├── CMakeLists.txt
│       └── src/
├── cmake/
│   └── aura-toolchain.cmake
├── configs/
│   └── streaming/
├── tools/
│   ├── build-aura
│   ├── build-host
│   └── deploy-aura
├── tests/
│   ├── protocol/
│   └── latency/
└── docs/
```

程序名称使用 `sender/viewer`，比 `master/slave` 更准确地描述视频数据方向。后续双向控制可以另建独立的控制通道。

## 4. Aura 发送端设计

### 4.1 采集与编码

首个版本使用单路 1280×720、60 fps、H.264：

```text
VI → VPSS crop/scale → VENC H.264 → UDP
```

编码器初始配置：

- Normal-P，无 B 帧和重排序；
- GOP 15～30；
- CBR 12～20 Mbit/s；
- `base:low_delay = 1`；
- VI/VENC wrap；
- `split:mode = BY_CTU`；
- `split:out = MPP_ENC_SPLIT_OUT_LOWDELAY`；
- 收到一个 slice packet 后立即交给网络线程。

当前 `aura-sender` 已实现这一条最小硬件路径，直接绑定 `VI → VENC`，不经过
VPSS，也不会把原始 NV12 图像带到用户态。配合 teleop MIA1321 内核补丁，默认值为
1280×1080、60 fps、20 Mbit/s H.264 CBR、GOP 15、VI/VENC wrap 256 行，以及
`VENC_SLICE_SPLIT_S` 的 mode 3、120 CTU。
当前默认`--output-mode frame`使用mode1，所有slice在同一个完整pack返回后立即发送；
`--output-mode slice`使用mode3按同一PTS聚合AU，等下一PTS确认结束后发送。
两者都标记完整AU，viewer收齐后立即提交VAAPI；旧无标记输入仍使用parser。
应用实现与实机对照见[2026-10-13优化记录](frame-boundary-optimization-2026-10-13.md)。
`--frames 0` 持续运行，其他 VI、VENC、分辨率和编码
参数都可以在命令行覆盖。

首板上以传感器最高模式和 ISP 当前输出一致的宽高测试：

```sh
/oem/usr/bin/aura-sender \
  --destination 192.168.1.2 --port 5000 \
  --width 1280 --height 1080 --max-width 1280 --max-height 1080 --fps 60 \
  --wrap-lines 256 --slice-ctus 120 --bitrate-kbps 20000 --gop 15
```

Aura 当前 low-delay VENC 配置每次 `RK_MPI_VENC_GetStream()` 回调给出一个 Annex-B
码流片段（mode3）；它不是FFmpeg可直接解码的完整H.264 access unit。
当前默认mode1得到整帧立即发送；接收端对有完整AU标记的payload绕过parser，
保留Annex-B parser兼容旧字节流。原先两道帧边界等待的实验参见
[2026-10-13探索记录](frame-boundary-wait-exploration-2026-10-13.md)。
不能将每个slice伪装成独立图像帧，也不启用曾导致AMD VAAPI兼容问题的
`AV_CODEC_FLAG2_CHUNKS`。

调优按整帧稳定基线、wrap、slice、减小 VI/VENC buffer 的顺序进行。每次只改变一组参数并记录 P50、P95、P99、丢帧和温度。

### 4.2 线程模型

建议保持很少的固定线程：

1. MPP stream 线程阻塞等待 VENC packet；
2. UDP 线程立即发送可用 packet；
3. 低频控制/统计线程处理参数和状态输出。

MPP 到网络线程之间使用有界环形队列。队列满时丢弃旧的未发送视频数据，让系统追赶最新画面，不允许延迟持续积累。

网络线程优先考虑 `sendmmsg()` 和 `iovec`，减少系统调用和内存复制。第一版先保证正确性，再决定是否需要实时调度优先级、CPU affinity 或 busy polling。

## 5. UDP 协议

单个 UDP datagram 固定上限为 1200 字节，避免普通 1500 MTU 网络上的 IP 分片。当前协议 v1 使用 40 字节、显式网络字节序的固定头，因而单包最多承载 1160 字节视频数据。每个包包含：

```text
magic / version
stream_id
frame_id
packet_id
packet_count
frame_size / fragment_offset / payload_size
flags（bit0 IDR，bit1 完整AU；所有分片一致）
capture_timestamp（`CLOCK_MONOTONIC` 域）
```

初期使用直接网线，不增加 FEC。接收端维护一到两个 frame slot：

- 包齐后立即提交解码；
- 新帧到达而旧帧仍不完整时丢弃旧帧；
- 不为了补齐旧画面继续增加 jitter buffer；
- 统计乱序、丢包、过期帧和 IDR 恢复时间。

协议需要显式版本号，并为将来的双路视频、控制消息和时间同步保留 `stream_id` 与保留字段。

`capture_timestamp` 只能在发送端和接收端共享同一单调时钟时计算发送到接收的延迟；本机回环测试满足该条件。实机的一向延迟需先通过 PTP 同步时钟，screen-to-screen 则以外部光学测量为准。

## 6. CachyOS 接收端设计

`host-viewer` 只在 CachyOS 宿主机编译和运行，不能在 Distrobox 内构建。CMake 会拒绝容器中的 viewer 配置，避免链接到 Ubuntu 的 FFmpeg、SDL 或 DRM 库。

`host-viewer` 不使用 Rockchip MPP。当前实现使用 FFmpeg `libavcodec` 的 VAAPI H.264 硬件解码，并且拒绝软件回退；初始化失败或输出不是 `AV_PIX_FMT_VAAPI` 即退出。后续可保留软件解码作为诊断对照，但不进入默认低延迟路径。

当前 CachyOS 主机的 AMD Radeon 880M/890M 可通过 `/dev/dri/renderD128` 使用 VAAPI。Vulkan 显示已使用以下链路：

```text
FFmpeg AV_PIX_FMT_VAAPI
  → libplacebo 将 VAAPI surface 派生为 DRM PRIME
  → Vulkan external-memory import
  → libplacebo fast renderer
  → Vulkan swapchain
```

该路径不把解码帧下载到 CPU。SDL3 仅创建 Wayland/X11 窗口和 `VkSurfaceKHR`，不使用 SDL renderer。libplacebo 使用单图形队列、关闭异步 compute/transfer，swapchain 首选 `MAILBOX`、深度为 1；若驱动不支持 MAILBOX，libplacebo 会退回 FIFO。每帧提交后调用 swapchain 同步，目的是避免展示端积压旧画面。

显示后端建议按以下顺序实现：

1. SDL/Vulkan 窗口，便于快速开发和分屏测试；
2. Vulkan/DRM 零拷贝路径；
3. 必要时使用 DRM/KMS 独占输出，绕过桌面合成器。

接收端默认采用 newest-frame-wins：如果显示赶不上接收速度，丢弃旧的待显示帧，不排队追赶。

界面显示帧号、码率、丢包、乱序、解码时间、显示帧率和当前队列深度，便于把视觉结果与程序统计对应起来。

## 7. 开发期构建和部署

Aura 程序必须使用 SDK 的 AArch64 工具链以及匹配的 MPP/RK MPI 头文件和库，不能直接在 CachyOS 上使用宿主编译器生成。

典型顺序为：

```sh
cd /home/ww/codings/aura-sdk
./build.sh media

cd /home/ww/codings/luckfox-aura-teleop
./tools/build-aura

# Check the sender ELF, SDK staging rootfs/OEM libraries, and generated images.
./tools/check-aura-runtime

# CI 或非相邻目录布局可以显式覆盖 SDK 路径。
AURA_SDK_ROOT=/path/to/aura-sdk ./tools/build-aura
./tools/build-host

# 主机回环：构建、协议单测、发送 90 帧假数据并由 viewer 重组。
./tools/test-transport-loopback

# CachyOS 实机 VAAPI 验证：生成 H.264 Annex-B 测试流并以硬件路径解码。
./tools/test-vaapi-decoder

# CachyOS 实机显示验证：VAAPI → DRM PRIME → Vulkan/SDL3 窗口。
./tools/test-vulkan-presenter

# CachyOS USB 摄像头闭环：/dev/video0 → H.264 UDP → VAAPI/Vulkan。
./tools/test-usb-camera-loopback
```

`tools/build-aura` 已在 Ubuntu 22.04 的 `aura-sdk` Distrobox 中验证：生成的
`build/aura-mpp/bin/aura-sender` 为 AArch64、使用 SDK 的 `librockit.so` 和
`librockchip_mpp.so.1`。目标端必须保留这两个媒体库及其动态依赖；虽然可以裁掉
内核声卡驱动和音频应用，当前 SDK 的 `librockit.so` 仍依赖 `libasound.so.2`，所以
在替换或重编该媒体库前不能从 rootfs 删除 ALSA 用户态运行库。

sender 的 ELF `RPATH` 固定为 `/oem/usr/lib`，与 SDK 把 `librockit.so`、
`librockchip_mpp.so.1` 和 `libdrm.so.2` 安装到 OEM 分区的方式一致；不会写入
开发机上的 SDK 绝对路径。因此把 `build/aura*/bin/aura-sender` 复制到
`/oem/usr/bin/` 或 `/userdata/` 后，都可以直接运行，不需要设置 `LD_LIBRARY_PATH`。

开发期通过以太网和 SSH 部署到 `/userdata`：

```sh
scp out/aura/teleop-sender \
  root@192.168.1.10:/userdata/teleop-sender

ssh root@192.168.1.10 \
  'chmod +x /userdata/teleop-sender && /userdata/teleop-sender --help'
```

这里使用的是 SCP/SFTP/rsync。UFS 是存储接口，不是以太网文件传输协议。`/userdata` 位于 eMMC 的持久化分区，适合频繁替换测试程序。NFS 可以作为可选调试手段，但不是首选部署路径。

## 8. 集成进 SDK 镜像

### 8.1 推荐方案：SDK app + OEM 分区

发送程序依赖 SDK media/MPP 构建产物，因此优先接入 `project/app`，而不是第一步就做成纯 Buildroot package。

集成 patch 包含：

1. 在 `project/app` 构建规则中增加 `TELEOP_SENDER`；
2. 从 `RK_TELEOP_SOURCE_DIR` 获取本仓库中的源码；
3. 使用 SDK toolchain、media 头文件和链接库编译；
4. 将程序安装到 OEM staging 目录；
5. 最终镜像内安装为 `/oem/usr/bin/teleop-sender`；
6. `overlay-luckfox-teleop` 添加可开关的启动脚本和配置文件。

Teleop BoardConfig 增加类似配置：

```sh
export RK_TELEOP_SOURCE_DIR="${SDK_ROOT_DIR}/../luckfox-aura-teleop/src/aura-sender"
export RK_APP_TYPE="${RK_APP_TYPE} TELEOP_SENDER"
```

当前工作区中两个仓库互为相邻目录，因此该默认路径可用；CI 或不同目录布局允许通过环境变量覆盖。源码的唯一正式副本仍保存在 `luckfox-aura-teleop`，SDK 只保存集成 patch。

BoardConfig 已启用 `RK_BUILD_APP_TO_OEM_PARTITION=y`，使用 OEM 分区可以单独更新应用，同时避免把频繁变化的程序硬编码到基础 rootfs。

### 8.2 可选方案：Buildroot `BR2_EXTERNAL`

当程序依赖关系稳定、MPP/RK MPI 库能够作为明确的 sysroot 依赖提供时，可以增加 `BR2_EXTERNAL` package，使 Buildroot 完成编译和安装。这种方式适合最终可复现发行，但首个版本会受到 SDK media 与 Buildroot 构建顺序的限制，因此不是当前优先路线。

不把编译生成的二进制直接提交进 Git，也不把唯一源码副本复制到 SDK sample 目录。

## 9. GStreamer 的保留用途

保留一套等价的 GStreamer 测试管线，用于：

- 验证 V4L2 摄像头格式和帧率；
- 验证 H.264 Annex-B 码流能否被标准工具解析；
- 与原生 UDP 协议对照延迟和丢包恢复；
- 在自研 viewer 未完成时快速查看画面。

正式发送端可选地提供 MPP → `appsrc` → RTP/UDP 模式，作为标准 RTP 兼容性对照，但极限延迟测试以原生 UDP 模式为主。

## 10. 实施轮次

### A. 协议与假数据

- 已完成：C++20 公共包头、packetizer、接收重组与 `sendmmsg()` UDP 发送；
- 已完成：协议单测覆盖头部序列化、坏 magic 和三分片乱序重组；
- 已完成：同机回环发送端和接收端可输出完整性与同一时钟域下的传输延迟统计；
- 待完成：在真实 H.264 Annex-B Access Unit 上测试乱序、丢包和 frame slot；
- 对协议解析和边界条件增加必要测试。

验收：本机回环和直连网卡下可持续传输，队列不会无限增长。

### B. Aura 单路整帧编码

- 从 SDK sample 提取最小 VI→VENC 路径；
- 先使用整帧 H.264 和直接 UDP；
- 通过 SCP 部署到 `/userdata`。

验收：720p60 连续运行，接收端无持续积压，并获得首份 screen-to-screen 基线。

### C. 低延迟输出

- 开启 wrap；
- 开启 slice low-delay；
- 降低 VI/VENC buffer；
- 调整线程优先级和 CPU affinity；
- 比较软件解码与 VAAPI。

验收：P50、P95、P99 均有记录，改动可以独立回退。

### D. SDK 镜像集成

- 增加 `project/app` 集成 patch；
- 程序安装到 OEM 分区；
- 增加启动脚本，但开发镜像默认允许手动启停；
- 完整执行 SDK 构建、刷写和重启验证。

验收：全新 eMMC 镜像启动后可以通过配置启动 sender，不依赖手工复制文件。

### E. 双路扩展

- 协议启用两个 `stream_id`；
- 建立双 VI/VENC channel；
- 接收端并排显示；
- 测量双路码率、同步误差、温度和长稳表现。

## 11. 延迟验证

软件内部记录以下时间点：

```text
VI frame/sequence
VENC first packet
VENC last packet
UDP first/last packet sent
receiver first/last packet received
decoder submit/output
display submit
```

软件时间戳用于定位延迟来源；最终 screen-to-screen 数字仍使用外部高速相机或双光电二极管测量。正式报告至少包含分辨率、帧率、codec、码率、GOP、链路速度、显示刷新率以及 P50/P95/P99。

## 12. 首个可运行版本的边界

第一版只完成：

- 单路 720p60；
- H.264；
- 直连千兆以太网；
- 自定义 UDP 分包；
- 一个 sender 和一个 viewer；
- SCP 部署；
- 基本丢包、码率和延迟统计。

H.265、双摄、FEC、远程控制、自动码率、自适应抖动缓冲和开机自动运行都在单路 H.264 基线稳定后增加。
