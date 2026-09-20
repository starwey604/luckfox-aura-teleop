# Luckfox Aura 双目摄像头低延迟调优方案

## 1. 实验目标

本项目使用 Luckfox Aura（RV1126B）和全局快门 MIPI 摄像头，先跑通单路、随后扩展到双路，用直接网线连接 CachyOS 接收机，测量机械臂双目遥操作场景的 screen-to-screen 延迟。

Aura 发送端只负责：

```text
Sensor → VICAP/VI → ISP/VPSS → VENC → UDP
```

画面在外部接收机显示，因此最终系统只保留启动、存储、调试、双路 MIPI、ISP/MPP、千兆以太网和必要的系统支持。音频、本机显示、无线网络、通用 USB 和无关服务将在验证后裁掉。

实物到达前准备可编译配置和分阶段测试方法；涉及传感器、PHY、频率、温度与内存峰值的参数必须上板测量后确定。

## 2. 已核实的硬件和 SDK 基线

- Luckfox Aura 使用 RV1126B，提供两个 MIPI CSI 接口、千兆以太网及 H.264/H.265 硬件编码能力。
- Linux 为 6.1.141，目标架构是 AArch64 Cortex-A53。
- 内核当前是 `CONFIG_PREEMPT_NONE=y`、`CONFIG_HZ_250=y` 和 `CONFIG_CC_OPTIMIZE_FOR_SIZE=y`。
- media 使用 Rockchip GCC 12.4，主要参数为 `-Os -march=armv8-a -mtune=cortex-a53`。
- Buildroot 为 2025.02.6，默认使用 `-Os` 和内部 GCC 13.3。
- Aura BoardConfig 当前使用 `RK_BOOTARGS_CMA_SIZE="128M"`，最终生成 Rockchip 专用参数 `rk_dma_heap_cma=128M`。
- 板载 RJ45 使用 Rockchip GMAC/STMMAC，DTS 为 RGMII、MDIO 地址 1；内核已启用 `CONFIG_STMMAC_ETH=y` 和 `CONFIG_STMMAC_ETHTOOL=y`。
- SDK 已提供 CSI0、CSI1 和双摄 DTSI。
- 内核已有 `CONFIG_VIDEO_MIA1321=y` 和 `drivers/media/i2c/mia1321.c`。

参考资料：

- [Luckfox Aura 官方规格](https://wiki.luckfox.com/Luckfox-Aura/Introduction/)
- [RV1126B 数据手册](https://wiki.luckfox.com/assets/files/Rockchip_RV1126B_Datasheet_V1.1-20250428-fb40d5e075643bb7fc9aa0acb0871ea7.pdf)
- [Rockchip MPP 开发文档](https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md)

## 3. ISG1321/MIA1321 模式与设备树

当前 MIA1321 驱动向 V4L2 注册三个 1280×1080 模式：

| 像素格式 | 分辨率 | 最大帧率 |
|---|---:|---:|
| RAW12 | 1280×1080 | 30 fps |
| RAW10 | 1280×1080 | 60 fps |
| RAW10 | 1280×1080 | 约 117 fps |

代码中虽然有一个名为 `mia1321_linear_10_1280x720_regs` 的寄存器表，但它实际被注册成 1280×1080、30 fps、RAW12。不能把它视为已经可用的原生 720p 模式。

SDK 现有设备树文件：

```text
rv1126b-luckfox-aura-cam-csi0.dtsi
rv1126b-luckfox-aura-cam-csi1.dtsi
rv1126b-luckfox-aura-dual-cam.dtsi
```

MIA1321 DTS 使用：

- I²C 地址 `0x60`；
- 26 MHz xvclk；
- 两条 MIPI data lane；
- CSI0：I²C4、reset GPIO4_B0；
- CSI1：I²C3、reset GPIO4_B1。

Luckfox 商品名 ISG1321 与内核驱动名 MIA1321 不一致。上板后必须验证 I²C 响应、`mia1321` probe、media graph、曝光控制和实际画面，确认模组确实兼容。

### 首次实机探测记录（2026-09-20）

在 Aura 的 CSI0 配置和一只 ISG1321/MIA1321 模组上，板端已经完成下列验证：

- `mia1321 4-0060` 成功读取芯片 ID，并报告 `Detected MIA1321(mis0400) sensor`；
- `/dev/media1` 已形成 `MIA1321 -> csi2-dphy0 -> mipi0-csi2 -> rkcif` 拓扑；
- `/dev/media2` 已形成 `rkcif -> rkisp-vir0 -> /dev/video13` 拓扑；
- `/dev/video13` 是 `1280x1080 NV12` 的 ISP mainpath 输出；Buildroot 镜像中已有 `media-ctl`、`v4l2-ctl`、`i2cdetect` 和 `gst-launch-1.0`。

首次采集时驱动开始 streaming，但没有返回帧，`mipi0-csi2` 持续报告 `MIPI_CSI2 ERR2:0x10000000`。断电重新插拔 FPC 后，该错误消失，说明原因是摄像头接口接触不可靠，并非 MPP、UDP 或 SSH。

复测命令为 `v4l2-ctl -d /dev/video13 --stream-mmap=3 --stream-count=120 --stream-to=/dev/null --verbose`。它完整返回 120 帧，每帧 `bytesused=2073600`，符合 1280x1080 NV12；帧间隔稳定在 30.738 ms，即约 32.53 fps，内核日志也没有新的 CSI 错误。该帧率高于模式表登记的 30 fps，支持后续单独修正 24 MHz 寄存器表与 26 MHz xvclk 配置不一致的判断；在修正前以实测 32.53 fps 作为延迟和吞吐测试的基线。

复测按以下顺序进行：

1. 断电后重新插拔并检查 FPC 的朝向、锁扣和 CSI0 插座；I²C 能工作不代表 MIPI 时钟和两条 data lane 都可靠连接。
2. 保持当前 DTS 的 CSI0、I²C4、`data-lanes = <1 2>`，避免在未确认硬件连接前切换到 CSI1。
3. 修正并复测传感器时钟与模式选择。当前默认 30 fps 寄存器表注释为 24 MHz 输入，而 DTS 和驱动给它提供 26 MHz MCLK；此外当前模式选择只按分辨率挑选，三个模式尺寸相同，导致 RAW10 60/117 fps 不能被格式协商可靠选中。
4. 每次修改后先重新运行上述 120 帧丢弃采集；只有它在规定帧数内退出且没有 CSI 错误，才进入 ISP 调参与 MPP sender。

首次尝试把 1280×1080 RAW10、标称 117 fps 的 26 MHz 寄存器表设为默认模式后，实测出现 CSI ECC/CRC/packet-ID 错误和 CIF size error，不能使用。当前 teleop 内核补丁改为默认使用同样以 26 MHz xvclk 工作的 RAW10 60 fps 表，并保留按 RAW 位深选择模式的修正。烧录后必须重新记录实际帧率、完整帧数与 CSI 错误计数；只有稳定通过后，它才是当前模组可用的最高帧率。117 fps 需要单独匹配传感器输出速率和 CSI D-PHY 参数后再尝试。

双摄 DTS 从厂商 `rv1126b-luckfox-aura-dual-cam.dtsi` 派生，重点验证两个 module index、CIF virtual channel、ISP 路由、reset 极性和同时启动能力。严格双目曝光同步可能需要模组硬件 trigger/sync，设备树和软件同时 `stream on` 不能保证微秒级同步。

## 4. 720p 与原始流带宽

未计封装的有效像素带宽为：

```text
带宽 = width × height × bits-per-pixel × fps
```

| 模式 | 有效像素带宽 | 千兆网判断 |
|---|---:|---|
| 1280×1080 RAW10 60 fps | 829.44 Mbit/s | 单路已接近极限 |
| 1280×720 RAW10 60 fps | 552.96 Mbit/s | 单路可行 |
| 1280×720 16-bit 容器 60 fps | 884.74 Mbit/s | 几乎没有余量 |
| 1280×720 NV12 60 fps | 663.55 Mbit/s | 单路可行 |
| 双路 1280×720 RAW10 60 fps | 1.106 Gbit/s | 不可行 |
| 单路 1280×720 RAW10 117 fps | 1.078 Gbit/s | 不可行 |

标准 MTU 下还要加入 RTP/UDP/IP、以太网头、FCS、前导码和帧间隙。单路 720p60 Packed RAW10 的实际线速约为 580–590 Mbit/s。

第一版 720p 应通过硬件裁剪或缩放得到：

```text
MIA1321 1280×1080
  → VICAP/ISP/VPSS crop或scale
  → 1280×720 NV12
  → VENC
  → UDP
```

这样可以降低编码和网络负载，但如果传感器仍输出完整 1080p，MIPI 输入带宽和传感器读出时间不会下降。真正的低延迟 720p 高帧率模式需要传感器原生 window/crop 寄存器配置和新的驱动 mode。

## 5. 原始 UDP 与压缩 UDP

### 5.1 原始流

原始流适合验证传感器、V4L2、网络和接收显示，不依赖编码器。缺点是带宽高、UDP 丢一个包就可能破坏整帧，而且发送完整 720p RAW10 帧本身约需 9.2 ms 线速时间。

适用范围：

- 单路 720p60；
- 双路 720p30；
- 降分辨率或裁剪后的诊断；
- 短时间、无丢包的直连测试。

### 5.2 H.264/H.265

压缩后每路只需要数 Mbit/s 到数十 Mbit/s，网络序列化时间可降到一毫秒以内。例如 12 Mbit/s、60 fps 平均每帧约 200 Kbit，在千兆网上只需约 0.2 ms。

因此增加硬件编解码并不必然增加端到端延迟。它增加几毫秒 codec 时间，却可以节省约 9 ms 的 720p RAW10 线路发送时间；如果按 slice 边编码边发送，还可以与采集和网络过程重叠。

## 6. H.264 与 H.265 选择

上板前对 720p60 硬件路径的工作估计如下：

| 阶段 | H.264 | H.265 |
|---|---:|---:|
| Aura 硬件编码 | 2–6 ms | 3–8 ms |
| CachyOS 硬件解码 | 1–3 ms | 1–4 ms |
| 打包、队列和调度 | 1–3 ms | 1–3 ms |
| 合计 | 4–10 ms | 5–12 ms |

这些数值必须通过实机时间戳复核。若管线等待完整帧、使用 B 帧、保留大队列或让 sink 按时钟同步，任一编码格式都可能额外增加 16.7–33 ms。

低延迟场景中的取舍：

| 项目 | H.264 | H.265 |
|---|---|---|
| 编码/解码延迟 | 通常更低 | 通常高约 1–4 ms |
| 同画质码率 | 较高 | 通常低约 20–35% |
| 兼容性 | 最好 | 较好 |
| 丢包后恢复和调试 | 较简单 | 依赖关系更复杂 |
| 直连千兆网双路 720p | 优先选择 | 作为对照 |

H.265 的高压缩优势在使用长 GOP、复杂参考结构和充分编码时间时更明显。关闭 B 帧、lookahead 和重排序后，其低延迟码率优势会缩小。

主方案先使用：

```text
H.264 Normal-P
无 B 帧
GOP 15～30
每路 12～20 Mbit/s
VI/VENC wrap
slice low-delay output
UDP/RTP
```

对照方案使用：

```text
H.265
无 B 帧和重排序
GOP 15～30
每路 8～14 Mbit/s
相同 wrap、slice 和接收端设置
```

## 7. MPP 低延迟设置

SDK 的 MPP 支持 H.264/H.265 slice 分割和低延迟输出：

```text
base:low_delay = 1
split:mode = BY_CTU 或 BY_BYTE
split:arg = 通过实测选择
split:out = MPP_ENC_SPLIT_OUT_LOWDELAY
```

低延迟模式让每个 slice 单独作为 packet 输出，不必等整帧码流完成。SDK sample 还提供：

```text
RK_MPI_VI_SetChnWrapBufAttr()
RK_MPI_VPSS_SetChnWrapBufAttr()
RK_MPI_VENC_SetChnBufWrapAttr()
```

调优顺序：

1. 先用整帧 VI→VENC 得到稳定基线。
2. 开启 VI/VENC wrap，默认可从 540 行开始。
3. 逐步测试 256、128、64 行，记录稳定性和延迟。
4. 打开 BY_CTU slice split 和 low-delay output。
5. 将 VI buffer、VPSS buffer 和 VENC stream buffer 逐步减到稳定下限。
6. 确保网络线程收到一个 slice 就立即发送，不重新拼成完整帧。

更小的 wrap line 和 slice 可以更早输出，但会增加中断、packet、header 和调度开销。最小值不一定是延迟最低且最稳定的值。

## 8. GStreamer 快速验证

当前 Buildroot defconfig 没有启用 GStreamer，但源码树包含 GStreamer 1.x、V4L2、UDP、RTP 和 parser。快速验证镜像启用：

```text
BR2_PACKAGE_GSTREAMER1=y
BR2_PACKAGE_GSTREAMER1_INSTALL_TOOLS=y
BR2_PACKAGE_GSTREAMER1_PARSE=y
BR2_PACKAGE_GST1_PLUGINS_BASE=y
BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_APP=y
BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_TYPEFIND=y
BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_VIDEOCONVERTSCALE=y
BR2_PACKAGE_GST1_PLUGINS_GOOD=y
BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_V4L2=y
BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_UDP=y
BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_RTP=y
BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_RTPMANAGER=y
BR2_PACKAGE_GST1_PLUGINS_BAD=y
BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_VIDEOPARSERS=y
```

同时启用：

```text
BR2_PACKAGE_LIBV4L=y
BR2_PACKAGE_LIBV4L_UTILS=y
BR2_PACKAGE_ETHTOOL=y
BR2_PACKAGE_IPERF3=y
BR2_PACKAGE_IPROUTE2=y
BR2_PACKAGE_TCPDUMP=y
```

标准 `v4l2src` 用于验证 V4L2 和原始 UDP。SDK 中没有发现现成的 Rockchip MPP GStreamer 编码插件；压缩路径应复用 RK MPI sample 或自有 sender，将 H.264/H.265 slice 交给 GStreamer `appsrc`/RTP/UDP，或者直接发送 UDP。

所有 GStreamer queue 最大一个 buffer，并设置为 downstream leaky；接收端关闭不必要的 clock sync，不使用大 jitterbuffer。官方 `queue` 支持在满时丢弃数据，避免实时管线持续积压：[GStreamer queue 文档](https://gstreamer.freedesktop.org/documentation/coreelements/queue.html)。

## 9. 内核低延迟配置

### 9.1 抢占与 tick

第一版 fragment：

```text
# CONFIG_PREEMPT_NONE is not set
CONFIG_PREEMPT=y
CONFIG_PREEMPT_BUILD=y
CONFIG_PREEMPT_COUNT=y
CONFIG_PREEMPTION=y
CONFIG_PREEMPT_RCU=y

# CONFIG_HZ_250 is not set
CONFIG_HZ_1000=y
CONFIG_HZ=1000
```

暂不启用 PREEMPT_RT。普通 PREEMPT 用于减少内核不可抢占区造成的调度长尾。`HZ=1000` 会增加 tick 开销，必须通过 P50/P95/P99 和 CPU 占用判断是否保留。

### 9.2 编译优化

当前内核使用 `CONFIG_CC_OPTIMIZE_FOR_SIZE=y`。功能基线稳定后单独测试：

```text
# CONFIG_CC_OPTIMIZE_FOR_SIZE is not set
CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y
```

这对应内核支持的 `-O2`。不为内核增加 `-O3`。当前 LTO 路径主要面向 Clang，而 SDK 使用 Rockchip GCC 12.4；第一轮不切换工具链和 LTO。

Buildroot 与 media 暂时保留 `-Os`，自有 sender 可以单独测试 `-O2`/`-O3`。

### 9.3 CPU、NPU 和媒体时钟

启动脚本在节点存在时设置：

```sh
echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
```

如果不运行推理，最终镜像应禁用 NPU，而不是让闲置 NPU 常驻 performance。若后续使用 NPU，先读取可用 governor 和频率，再选择 performance 或 userspace 最高安全频率。

视频链路更相关的是 ISP、VENC/MPP、内存带宽和 CPU。SDK 可通过 `/proc/mpp_service/rkvenc/clk_core` 调整 VENC 时钟，但必须上板读取可用频率，并同时监控温度与 throttling。

### 9.4 CMA

teleop BoardConfig 使用：

```sh
export RK_BOOTARGS_CMA_SIZE="256M"
```

实际生成：

```text
rk_dma_heap_cma=256M
```

这是 Rockchip DMA heap CMA。首轮不要再同时加入 `cma=256M`，否则可能形成两个预留区。启动后检查：

```sh
cat /proc/cmdline
grep -i cma /proc/meminfo
dmesg | grep -iE 'cma|dma.heap|mpp'
```

CMA 主要降低连续内存分配失败风险，本身不会自动减少正常路径延迟。最终大小依据内存型号、双摄和 VENC 峰值占用调整。

## 10. 内核保留与裁切

必须保留：

- ARM64、SMP、中断、timer、DMA、IOMMU、DMA-BUF heaps；
- UART、devtmpfs、procfs、sysfs，调试阶段保留 debugfs；
- eMMC/SD、DW-MMC、ext4 和启动分区支持；
- IPv4、UDP、Unix sockets，抓包阶段保留 packet sockets；
- STMMAC、Rockchip GMAC glue、MDIO、PHYLIB 和板载 PHY；
- GPIO、pinctrl、I²C3/I²C4、clock、regulator 和 RK801 PMIC；
- media controller、V4L2 subdev、VB2、CIF、CSI2 D-PHY、ISP、VPSS；
- `CONFIG_VIDEO_MIA1321=y`；
- Rockchip MPP、DVBM、DMA heap CMA；
- RGA，直到确认不需要格式转换和缩放；
- thermal、watchdog、CPUFreq 和必要 devfreq。

第一轮可裁：

- 其他摄像头 sensor 驱动；
- ALSA、ASoC、codec、PDM、I²S、SPDIF 和 USB audio；
- DRM、VOP、DSI、RGB、panel、framebuffer 和 backlight；
- touchscreen 和无关 input；
- Bluetooth、RFKILL、Wi-Fi 和 SDIO Wi-Fi；
- netfilter、NAT、bridge、TUN 和 VETH；
- UVC、USB mass-storage 和无关 gadget function。

后期再裁：

- USB host/OTG/DWC3，必须先验证串口和网口恢复路径；
- kernel modules，所有必要驱动改为 built-in 后再评估；
- debugfs、ftrace、perf、动态调试和符号；
- SSH、IPv6 和诊断工具；
- NPU/RKNPU；
- 未使用的 RTC、按键和 LED，散热相关支持继续保留。

AIISP、FEC、RGA 和 VPSS 是否删除要依据最终 media graph。DTS 的 `status` 与 Kconfig 裁切必须同步，避免留下持续 probe 失败的节点。

## 11. Buildroot 裁切

开发镜像保留：

- BusyBox、动态链接器和基本 shell；
- DHCP/静态 IP、iproute2、iperf3、ethtool；
- OpenSSH；
- v4l2-utils、media-ctl、tcpdump；
- 最小 GStreamer core/base/good/bad parser；
- Rockchip ISP/MPP/RKAIQ 库和 IQ 文件；
- teleop sender 和启动脚本。

验证后删除：

- ALSA library、utilities 和音频配置；
- FFmpeg 全家桶；
- Python 及未使用的 Python 包；
- Samba、Bluetooth、Wi-Fi、hostapd 和 wpa_supplicant；
- LVGL、libdrm 和本机显示应用；
- RKAI、web backend、UVC_TINY 和 RKIPC Web/RTSP 服务；
- fio、sysstat、Rockchip test 和生产镜像不需要的工具。

快速验证阶段暂时保留 RKIPC/RK MPI sample 作为 VI/VENC 参考。自有 sender 跑通后，将 `RK_APP_TYPE` 收敛到 teleop 所需的最小集合。

## 12. 网络与接收端

直接连接建议使用静态地址，例如：

```text
CachyOS: 192.168.50.1/24
Aura:    192.168.50.2/24
```

先验证：

```sh
ethtool eth0
iperf3
ethtool -k eth0
cat /proc/interrupts
```

调优项包括：

- 对比 GRO 开/关的 P99 延迟、CPU 和丢包；
- 将采集、编码、发送线程固定到合适 CPU；
- 必要时分离 GMAC、CSI/ISP 和 VENC IRQ；
- UDP socket buffer 只设置到不丢包所需大小；
- 不使用大接收缓存掩盖管线积压；
- 使用 `SO_TIMESTAMPING` 或 monotonic timestamp 分解软件路径。

当前 CachyOS 接收机有 Radeon 880M/890M、RTL8125 2.5GbE、144.02 Hz DP 显示和 100 Hz HDMI 显示。Aura 会把链路限制在 1GbE。极限延迟测试优先使用 144 Hz DP 显示、硬件解码和全屏直接显示。

当前 DP 输出存在 1.35 缩放和旋转。正式测试时尽可能使用 scale 1、无旋转和 direct scanout；缩放、旋转及桌面合成可能增加一次或多次刷新等待。

## 13. 预计延迟目标

60 fps 低延迟压缩链路的预算：

| 阶段 | 预计延迟 |
|---|---:|
| 等待曝光及传感器读出 | 8–20 ms |
| VI/ISP/VPSS | 3–10 ms |
| H.264 编码 | 2–6 ms |
| 直接以太网 | <1 ms |
| AMD 硬件解码 | 1–3 ms |
| 144 Hz 合成及显示扫描 | 4–14 ms |

阶段性目标：

| 实现 | Screen-to-screen 目标 |
|---|---:|
| 初版 GStreamer | 70–150 ms |
| GStreamer + RK MPI，队列一帧 | 45–80 ms |
| 60 fps 完整优化 | 35–60 ms |
| 117 fps + wrap/slice + 144 Hz | 20–35 ms |

推荐验收指标：

```text
第一阶段：P95 < 100 ms
第二阶段：P95 < 60 ms
冲刺目标：P50 25～40 ms，P95 < 50 ms
```

低于 20 ms 不能作为 60 fps 系统的预期目标。内核裁切和 governor 主要改善抖动与 P95/P99；决定平均延迟的主要因素是传感器帧率和曝光、wrap/slice 流水、缓冲深度、编码器参考结构、接收端解码以及显示扫描。

## 14. 测量方法

screen-to-screen 最终使用高速摄像机或光电传感器测量：

1. 输入屏显示黑白跳变或高精度计时图案。
2. ISG1321 拍摄输入屏。
3. Aura 采集、编码并通过 UDP 发送。
4. CachyOS 解码并显示。
5. 高速相机同时拍到输入屏和输出屏。
6. 逐帧统计 P50、P95、P99 和最大延迟。

同时记录内部时间戳：

- sensor/VI frame sequence 和 PTS；
- VENC 输入和第一个 slice 输出；
- UDP 第一包/最后一包发送与接收；
- decoder 输入和输出；
- presentation timestamp。

内部时间戳用于定位瓶颈，高速摄像机结果作为最终端到端指标。测试过程中还要记录丢帧、码率、UDP 丢包、CPU 占用、实际频率、CMA 峰值和温度。

## 15. 首板到手后的执行顺序

1. 保存原厂镜像和完整串口日志。
2. 确认内存、存储和 PHY ID。
3. 使用 `iperf3` 验证千兆网和 UDP 丢包基线。
4. 接一只 ISG1321，检查 I²C `0x60`、`mia1321` probe 和 media graph。
5. 枚举 1080p30/60/117 模式并做纯采集丢帧测试。
6. 用 GStreamer 验证单路原始 UDP。
7. 用 RK MPI 跑通 VI→VENC→文件，测纯编码延迟。
8. 接入 UDP sender，先测试 H.264，再以相同条件测试 H.265。
9. 开启 VI/VENC wrap 和 slice low-delay，逐步缩小 buffer。
10. 接第二只摄像头，切换双摄 DTS，验证并发与同步。
11. 分别测试 PREEMPT、HZ=1000、performance 和 256M CMA。
12. 最后应用 kernel 和 Buildroot 裁切，并做长时间回归。

每一步保留上一版可启动镜像。传感器模式、DTS、CMA、内核调度、编码器和大规模裁切不要在同一轮同时改变。
