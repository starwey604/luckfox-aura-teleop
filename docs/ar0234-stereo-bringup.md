# 两路 AR0234 720p：配置与实机验证

2026-10-12，双 DECXIN-3478V1 AR0234、720p 同时采集和回传已验证。
双路 **暂时关闭 VI/VENC wrap**，仍使用 MPP H.264、DMA buffer、slice mode 3
和本机 VAAPI + SDL3/Vulkan。两路真实场景各 1,200 帧解码显示通过，网络零丢帧。
不能将本次短时验证当作长时间运行或 screen-to-screen 延迟验收。

## 镜像改动

此前单摄 AR0234 配置删除第二路 sensor、关闭 CSI1 DPHY/CIF/ISP/VPSS，
所以增加独立 stereo preset，保留原单摄配置：

```sh
./tools/select-teleop-board ../aura-sdk ar0234-stereo
distrobox enter aura-sdk -- bash -lc \
  'cd /home/ww/codings/aura-sdk && ./build.sh all'
```

BoardConfig：`BoardConfig-EMMC-RK801-Luckfox_Aura-Teleop-AR0234-Stereo-Buildroot.mk`。
DTS：`rv1126b-luckfox-aura-ar0234-stereo.dts`，复用单摄已验证的 CSI0，再启用 CSI1。

| 项目 | camera0 | camera1 |
|---|---|---|
| 板上 CSI 接口 | CSI0/H3 | CSI1/H4 |
| DT I²C alias | i2c4 | i2c3 |
| 本次实机 client | 4-0010 | 3-0010 |
| 芯片 ID | 0x0a56 Bayer | 0x0a56 Bayer |
| 模组 | DECXIN-3478V1、24 MHz、3.3 V | 同左 |
| MIPI | 2-lane、900 Mbps/lane、连续时钟 | 同左 |
| DPHY/CIF | dphy0 / rkcif-mipi-lvds | dphy3 / rkcif-mipi-lvds2 |
| ISP | rkisp-vir0 | rkisp-vir1 |
| 本次 sensor node | /dev/v4l-subdev3 | /dev/v4l-subdev6 |
| 本次 mainpath | /dev/video24 | /dev/video32 |
| VI dev/pipe/channel | 0/0/0 | 1/1/0 |
| VENC / stream_id | 0 / 0 | 1 / 1 |

两个 `0x10` 位于不同 I²C 总线，不存在地址冲突。节点编号不是永久接口，
诊断与启动脚本根据实际 DT alias 和 sysfs/media 查询 sensor。
SSH、静态 IP、Buildroot 工具、AR0234 内置驱动、PREEMPT/HZ 配置无需新增包；
保留 `rk_dma_heap_cma=256M`，实机确认 CMA 已生效。

SDK kernel 提交 `b17875fb3`，BoardConfig 提交 `83389e1`。
整套镜像于 14:46–14:47 在 Ubuntu 22.04 Distrobox 构建成功，已由用户烧录。
最终 boot.img 的 DTB 五项检查通过，提取的内核与构建产物一致，MPP/Rockit
模块 vermagic 匹配 PREEMPT 内核。主要镜像 SHA-256：

```text
57b4c2ae0654ab1eb7128f7faa3f0d944826ef0d2e468da325861d8fcc9dd3d5  boot.img
f9f4270edc908031c739d247a0533883f8c92cf82310fe935bdadfb8271ffaa0  rootfs.img
181d86d22229ca7fe239590e4b92845217bf04ef277d1c421f734768eb71f03d  oem.img
```

后续应用程序和脚本修复通过 SSH 部署，**不需要再次烧录镜像**。

## 脚本与运行方式

`tools/deploy-aura` 现在同时部署 sender、`diagnose-ar0234` 和
`run-ar0234-stereo` 到 `/userdata`。本轮已直接完成部署。

`run-ar0234-stereo` 会：

- 拒绝已有摄像头程序占用节点的情况；只管理自己启动的进程。
- 在改控制参数前，先检查两颗芯片 ID、驱动绑定和 sensor node。
- 两路应用 exposure=735、analogue_gain=32、digital_gain=512、test_pattern=0。
- 第一颗 sender 完成初始化并输出 ready 日志后，再启动第二颗；之后两路并发。
- 分配独立的 camera/VI/VENC/stream ID，固定输出 1280×720。
- 默认 `WRAP_LINES=0`，保存每路日志；Ctrl+C/TERM/HUP 清理自己启动的 sender。
- 有限帧数时等待两路完成；任一路失败会停止其余子进程。

本机 viewer（CachyOS 原生运行）：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./build/host/bin/host-viewer \
  --port 5000 --dual-stream --frames 0 --timeout-ms 3600000 \
  --window-width 1080 --window-height 1216 --borderless
```

已旋转的竖屏内，stream 0 在上、stream 1 在下。
`--window-height` 是整个窗口高度，不会自动乘二。

SSH 到板上启动：

```sh
/userdata/run-ar0234-stereo
```

检查但不启动/修改控制值：

```sh
/userdata/run-ar0234-stereo --dry-run
```

先做两路同时采集，再做有限帧数编码：

```sh
/userdata/run-ar0234-stereo --capture-only --frames 600
# 本机 viewer 此时使用 --dual-stream --frames 1200。
/userdata/run-ar0234-stereo --frames 600
```

可选环境变量：DESTINATION、PORT、FPS、BITRATE_KBPS、EXPOSURE、ANALOGUE_GAIN、
DIGITAL_GAIN、REPORT_DIR、STARTUP_TIMEOUT、H264_DIR、WRAP_LINES。
`H264_DIR=/userdata/test` 将分别保存 camera0.h264 和 camera1.h264。
`FPS` 是 VENC rate-control 参数，**不会修改 sensor 时序**，不能靠它保证降低
摄像头采集负载。当前 sensor 仍约 99.23 fps；任何真正降采集帧率的测试应另行
通过 sensor frame interval/VBLANK 配置并重新测量。

原 `tools/test-dual-camera-stability` 专门针对 MIA1321 的 1280×1080 和 module
参数，不直接用于本次 AR0234；使用上述 `--capture-only` 与逐路报告。

## 发现的双路 wrap 错图与修复

最初保持两个 sender 的默认 wrap=256，两路各 600 帧收到且硬件解码正常，
但内容检查失败。给 camera0 竖彩条、camera1 渐变彩条后，camera0 的帧曾呈
纯绿色，后续出现 camera1 的重复条带。单独 ISP 采集 camera0 的彩条正常。

在相同双路拓扑下关闭 wrap，camera0 与 camera1 恢复完整、独立的各自测试图，
两路 H.264 全文件解码 warning 日志为空。实机日志显示 VENC channel `online 0`，
CIF 使用正常 DDR 路径。证据定位到当前 SDK 的双路 wrap 路径；不能据此声称
RV1126B 硬件不支持双路 wrap，SDK kernel 中也有 dual-wrap 支持代码。
其具体调度/缓冲问题留待单独修复。

sender 新增 `--wrap-lines 0`，以跳过 VI/VENC wrap 设置使用默认整帧缓冲；
此 SDK 拒绝向 wrap setter 传 bEnable=false，因此不能用一次禁用调用替代。
普通单摄 sender 的默认 wrap=256 保持不变，双摄启动脚本默认关闭。
这一选择可能增加端到端延迟，必须重新做光学 screen-to-screen 测量。

## 实机测试结果和限制

同时 V4L2 采集，各 600 帧：

| camera | 稳定段 fps | 间隔最小/最大 | 初始化 seq 缺口 |
|---|---:|---:|---|
| 0 | 99.23240 | 10.032 / 10.124 ms | seq 2→24，缺 21 帧 |
| 1 | 99.23134 | 10.025 / 10.128 ms | seq 2→24，缺 21 帧 |

稳定段从第 40 个返回帧之后统计。没有持续 seq 缺口，初始化仍含商家表的
stream-on/off 等待。vblank≈286 µs 的 online ISP 警告仍存在，不声称初始化零丢帧。

关闭 wrap 的不同测试图验证：每路 300 帧硬件解码，合计 600 帧显示，
网络 malformed/duplicates/dropped 全 0，实际图像各自正确。

曾在一次同时启动两个 sender 的真实场景测试中失去 SSH/ARP 响应，用户确认
没有拔插，并手动重启开发板。pstore 为空，保留日志停在 SDK 初始化阶段，
无法确定是并发初始化、此前 wrap 状态遗留还是其它 kernel/SDK 问题。
不得把这次卡死记录抹掉或把根因宣称已经完全解决。

重启后，脚本改为逐路等待初始化完成，并保留双路并发编码：

- 真实场景每路 300 帧，通过编码、UDP、VAAPI 和 Vulkan；两路照片显示不同视角。
- 停止后再次启动，每路 1,200 帧，通过，SSH 仍正常，内存 available 约 917 MiB。
- 两个真实场景 H.264 样本全文件解码 warning 日志为空。

最后一轮：

```text
viewer: access_units=2400 datagrams=49297 bytes=57783188 malformed=0 duplicates=0 dropped=0
viewer stream 0: decoder: frames=1200 hardware_frames=1200
viewer stream 1: decoder: frames=1200 hardware_frames=1200
presenter: frames=2400
```

这是短时与停止/重启基线，尚未验证小时级稳定性、双目曝光同步或新的光学延迟。
测试进程均已退出，测试图恢复 Disabled，亮度参数保留在两颗 sensor 的运行时
控制值中；脚本下次启动会重新应用，重启无需手动记忆 node 编号。

图片：`captures/ar0234-stereo/camera0.png`、`camera1.png`；
不同测试图：`no-wrap-pattern0.png`、`no-wrap-pattern1.png`。
样本受 ignore 排除，未提交摄像头画面。
日志：`logs/ar0234-stereo/`，包含初始化卡死前后的记录、捕获统计、逐路解码和 viewer。

自动化验证：Ubuntu Distrobox 内 launcher 6 项、diagnostic 12 项、stereo DTB 5 项
通过；sender 交叉编译/runtime 检查通过。CachyOS 原生双 stream transport loopback
与协议测试通过，180 个合成 access unit、零 malformed/dropped。
