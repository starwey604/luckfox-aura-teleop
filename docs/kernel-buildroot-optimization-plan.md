# 双 AR0234：内核、Buildroot 与视频链路优化计划

2026-10-12。依据当前 SDK 有效配置、生成的根文件系统、ELF 依赖和程序源码审计。
目标为双路 AR0234 1280×720、约 99.23 fps → MPP H.264 → 直连千兆网 → CachyOS VAAPI/Vulkan。
本轮只做探索、配置依赖验证和基线归档，没有修改正在使用的 SDK 配置，也没有构建或烧写新的镜像。

> 后续首轮 [软件性能记录](software-performance-baseline-2026-10-12.md) 已核对运行时状态：CPU policy 和 DMC
> devfreq 节点实际不存在，并已实现可关闭的局部阶段计时。光学测试暂缓，实机双路编码失联仍待排查。

建议先测现有镜像，再按“用户空间服务 → 音频/无线/网络功能 → 显示/NPU → USB → 调度与网络 → 编译器”的顺序推进。
裁剪的直接收益主要是镜像体积、启动时间、后台负载和内存；端到端延迟是否下降必须由光学测试判断。
目前更值得验证的延迟因素是 DDR governor、程序中的完整帧等待、接收/显示串行处理，以及 NIC 中断合并。

## 1. 当前有效配置与基线定义

配置来源不是原厂 defconfig 的猜测，而是以下构建产物：

- `../aura-sdk/sysdrv/source/objs_kernel/.config`，以及 `init/.main.o.cmd` 的实际编译命令。
- `../aura-sdk/sysdrv/source/buildroot/buildroot-2025.02.6/.config`。
- `.BoardConfig.mk` 指向 `BoardConfig-EMMC-RK801-Luckfox_Aura-Teleop-AR0234-Stereo-Buildroot.mk`。
- `output/out/rootfs_glibc_rv1126b`、`output/out/oem`、media 库和板端程序的 ELF 依赖。

源码 revision、有效选项与镜像/程序 SHA-256 见 [审计快照](baselines/2026-10-12-sdk.json)。

| 项目 | 当前实际状态 | 本轮判断 |
|---|---|---|
| 内核/工具链 | Linux 6.1.141，Rockchip GCC 12.4.0，GNU binutils 2.40 | 保留为首轮编译基线 |
| 抢占/tick | `CONFIG_PREEMPT=y`、`CONFIG_HZ=1000` | 已启用，不能再计作新增收益 |
| 高精度计时/tickless idle | `HIGH_RES_TIMERS=y`、`NO_HZ_IDLE=y` | 保留；1000 Hz 不等于每个网络包等一个 tick |
| 内核编译 | `CC_OPTIMIZE_FOR_PERFORMANCE=y`，实际 `-O2`，`LTO_NONE=y` | 没有内核 O3/LTO |
| CPU governor | 编译默认 interactive，启动脚本尝试改为 performance | 需上板读回，不只检查脚本存在 |
| DDR/devfreq | DMC devfreq 已启用；`DEVFREQ_GOV_PERFORMANCE=n` | 现有脚本写 performance 可能失败且错误被忽略 |
| 连续内存 | BoardConfig 使用 `rk_dma_heap_cma=256M` | 保持，不额外叠加 `cma=256M` |
| 摄像头 | AR0234、MIA1321 和十余种无关 sensor 都启用 | AR0234 专用配置只保留所需 sensor |
| 视频核心 | CIF、ISP V35、VPSS V20、AIISP、RGA、DVBM、DMA heaps 启用 | 首轮全部保留，不能只按应用是否调用来删 |
| MPP 驱动 | 内建 `ROCKCHIP_MPP_SERVICE=n`；实际使用外置 kmpp/Rockit 模块 | 不要误把这个 n 当作编码驱动缺失 |
| 音频/显示/USB/无线 | SOUND、DRM/FB、USB host/gadget、Wi-Fi、Bluetooth 均启用 | 明确裁剪对象 |
| 网络 | IPv6、netfilter/bridge/TUN/VETH、RPS/XPS/BQL 已启用 | 静态直连可去掉无关功能，保留 TCP/UDP/SSH |
| qdisc | `CONFIG_NET_SCHED=n` | 当前不能直接使用 `tc ... fq/fq_codel` |
| 调试 | debugfs、IKCONFIG、KALLSYMS、DWARF 已启用；FTRACE/function tracer 未启用 | DWARF主要占构建/调试文件，不能宣称删除就降低运行延迟 |
| Buildroot | 2025.02.6，glibc、GCC 12.4、`BR2_OPTIMIZE_S=y`、无 LTO | 用户空间主要为 `-Os` |
| 自有 sender/viewer | CMake Release 实际均为 `-O3 -DNDEBUG` | 再“启用 O3”不会改变当前这两个程序 |
| media | `-Os -march=armv8-a -mtune=cortex-a53` | 与内核、Buildroot 是不同构建路径 |

有效内核中有 1292 个 y/m 选项；Buildroot 有 410 个启用的 `BR2_PACKAGE_*` 符号。
后者包括包内功能开关，**不是 410 个独立安装包**。
板端 SSH 在本次审计时两次关闭连接，因此本轮没有取得新的运行时 governor、NIC 参数或延迟读数。
上一轮双路 1200 帧/路通过、零 UDP 重组丢帧的记录仍是已有证据，不能代替此次长时基线。

## 2. 已发现的裁剪依赖

### 音频驱动可以删，ALSA 运行库暂时保留

`src/aura-sender/CMakeLists.txt` 显式链接 `libasound`。
更关键的是当前 `librockit.so` 的 `DT_NEEDED` 本身包含：

```text
libasound.so.2
librockchip_mpp.so.1
libdrm.so.2
libm.so.6
libc.so.6
```

所以删除内核 `CONFIG_SOUND` 和 ALSA utilities 是合理的第一步；直接删除 `libasound.so.2` 会破坏加载。
`media/cfg/cfg.mk` 还有 `CONFIG_RK_ROCKIT_DEP_ALSA=y`、`CONFIG_ALSA_LIB=y`。
后期只有在获得真正不依赖 ALSA 的 Rockit 构建、重新检查 ELF 依赖并实机验证后，才能删除运行库。
仅删除 sender 的显式链接不能消除 Rockit 的间接依赖。

### 本机显示可删，libdrm 与 DRM 内核分别处理

当前 `librockit.so` 依赖 `libdrm.so.2`，所以这个**用户空间库仍保留**。
内核 VOP/DSI/panel/framebuffer 不负责远端显示，可以删除。
内核 RKNPU 当前选择 DRM GEM；本项目没有推理任务，先关 RKNPU，再关 DRM/FB。
Kconfig 已验证这组选择能够保留摄像头、RGA 和 DMA heaps，但仍须完整重建外置模块并上板检查初始化路径。
不要把 CachyOS viewer 的 SDL3/Vulkan/VAAPI 库一起裁掉；本计划裁剪的是 Aura 镜像。

### Rockit 核心模块不能按“未调用功能”随意拆除

`nm -u rockit.ko` 可见对 `aiisp_init/exit`、`rockit_rkisp_init/exit`、
`rockit_rkvpss_init/exit`、`vpss_init/exit`、`vrga_init/exit` 的引用。
当前 SDK 的 Rockit 是 `release_rockit-ko_rv1126b_arm64_asm` 发行形式，许多实现是 `.S`，并非完整可重优化的 C 源码。
因此首轮保留 AIISP、VPSS、RGA、DVBM 和五个核心模块
`kmpp`、`kmpp_smart`、`rockit_osal`、`rockit_base`、`rockit`。
后续根据符号、probe、设备打开和实际 media graph 判断哪些能进一步拆分。

### USB Linux 功能与烧录路径分开

最终可关闭 Linux USB host/gadget、UVC、UAC、mass-storage、ADB/MTP；
同时停止 `S50usbdevice` 和 UVC 应用，否则启动脚本会继续建立已经不存在的 gadget function。
Loader/Maskrom 烧录由 BootROM/loader/U-Boot 路径处理，和 Linux USB gadget 是不同阶段。
本轮不改 loader、U-Boot、分区布局或物理供电；USB 裁剪后仍要由用户实际验证按键进入 Loader/Maskrom 的恢复流程。
USB PHY/共享电源节点要按 DTS 依赖确认，不通过删除所有带 `usb` 字样的节点来处理。

### IPv6、I²C 工具与静态网络

板端 sender 发送 IPv4 地址，但公共接收器使用 `AF_INET6` + 双栈 socket；
板端若只运行 sender，可以评估去掉 IPv6，主机目前仍需 IPv6 socket 支持。
第一轮两边都保留 IPv6，后续先加显式 IPv4 接收路径再验证，避免影响测试工具。
保留 TCP、Unix sockets、packet sockets、PHYLIB/MDIO、GMAC、SSH；视频用 UDP 不代表 SSH 可以不要 TCP。

当前生成的 Buildroot `.config` 没有 `BR2_PACKAGE_I2C_TOOLS=y`，但最终 rootfs 中有 SDK 补充安装的 i2c 工具。
精简配置应显式启用这个包，并保持 `RK_ENABLE_I2C_TOOLS=y`，最后核对打包后的 `i2ctransfer`。
`dhcpcd` 当前承担已有静态地址配置；首轮保留，不能只因“没有 DHCP”就删除。

## 3. 分阶段裁剪

新增独立的 `ar0234-stereo-minimal` BoardConfig 和 Buildroot defconfig，保留现有 `ar0234-stereo` 作为回退。
每阶段生成独立内核 fragment，通过现有 SDK patch stack 保存；不在生成的 `.config` 中长期手工改参数。
Buildroot 的维护位置是 `sysdrv/tools/board/buildroot/`；BoardConfig、overlay 与 post-build 位于
`project/cfg/BoardConfig_Aura/`。media 和 `sysdrv/drv_ko` 的修改涉及其他独立 Git 项目时，必须纳入 manifest/series。

| 阶段 | 实际改动 | 验收及保留内容 |
|---|---|---|
| B0 | 当前镜像和程序，先测量 | 不改变摄像头表、DTS 路由、曝光和编码参数 |
| S1 | 精简后台服务、Buildroot 大包及 OEM 应用 | 用同一个内核判断用户空间收益；SSH、双摄、诊断工具和运行库完整 |
| K1 | 音频、无线、无关 sensor、netfilter/bridge/TUN/VETH/PPP | USB/显示先保留；主网卡、摄像头和模块重建通过 |
| K2 | 关闭 NPU，再关闭 DRM/VOP/DSI/panel/FB/backlight | libdrm 运行库保留；AIISP/VPSS/RGA 保留 |
| K3 | 关闭 Linux USB host/gadget，裁去相关服务和无关文件系统/input | 用户验证 USB 恢复路径；串口、SSH、eMMC 和供电支持保留 |
| N1 | governor、IRQ/线程亲和性、NIC/UDP 调优逐项 A/B | 不在此阶段改摄像头时序或同时切换编译器 |
| C1 | GCC 调优 / Clang / ThinLTO 分开实验 | 全套内核+外置模块重建及长时测试，不能只验 vmlinux |
| P1 | 合并实测有益设置，清理开发包和启动流程 | 稳定后再决定哪些诊断功能从生产镜像移除 |

### S1：Buildroot 与 SDK 打包

先保留 BusyBox、bash、glibc/动态链接器、C++ runtime、pthread/dl、eudev、OpenSSH/OpenSSL、静态网络、
`iproute2`、`ethtool`、`iperf3`、`tcpdump`、`v4l2-ctl`、`media-ctl`、`i2ctransfer`、必要模块加载和 OEM 挂载。
保留 media 所需的 `librockit`、MPP、`libasound`、`libdrm`，第一轮还保留 RGA/RKAIQ 及被实际加载的 IQ 文件。

删除或关闭：

- `BR2_PACKAGE_ALSA_UTILS`、FFmpeg/ffplay/SDL2、GStreamer 及插件；现在自有 sender 不使用它们。
- Python3 及 Python 包、BlueZ、hostapd/iw、dnsmasq、iptables、PPP、USBmount、Android tools、NTFS。
- 板端 git、ripgrep、压缩工具全集、编辑器/补全、串口终端、无关 input 服务，按诊断需要保留少量工具。
- BoardConfig 的 Wi-Fi/Bluetooth、web backend、LVGL、Rockchip test、fio；sysstat 在测量期可保留。
- `RK_APP_TYPE` 中 RKIPC/RKAI/UVC 应用，替换为项目自己的明确启动流程，不依赖空值触发 SDK 的 RKIPC fallback。
- `S35iptables`、`S40bluetoothd`、`S50usbdevice`、`S80dnsmasq`、`S99input-event-daemon`；
  OEM 中 `S50fcgiwrap/S50nginx`；删除测试自动重启入口 `S99_auto_reboot` 及对应测试脚本。

不能只改 Buildroot：BoardConfig 仍继承原厂的 web/RKAI/UVC/Wi-Fi 开关，media 顶层还遍历子目录 Makefile，
OEM 又会补充程序和库。需要同步处理应用选择、media 安装规则、overlay、模块安装与生成启动脚本。
模块加载应改成一份最小明确清单，保留五个核心模块及其真实依赖，去掉无关 sensor/Wi-Fi/motor 的加载。
`insmod_ko.sh` 还会修改 CIF/ISP 参数并暂停/恢复 udev 队列，不能用只含五行 insmod 的脚本草率替换。

OEM 中单 `librockchip_mpp.a` 就约 42 MiB，Python 库目录约 35 MiB；
静态开发库、samples、测试数据和未用模型可以从目标镜像去掉，**开发 staging 中仍保留**。
先对运行时 `DT_NEEDED` 和 Rockit 的 `dlopen` 路径做闭包检查，再决定 `librknnrt`、IVA 模型等的去留。
删除包需要干净的 Buildroot 输出和干净 rootfs/OEM staging；`olddefconfig` 或增量 make 不保证卸载旧包。
SDK 创建 Buildroot 时会重新解压源码，临时改解压目录也不会可靠持久化。
这一点见 [Buildroot 重建说明](https://buildroot.org/downloads/manual/manual.html#full-rebuild)。

### K1/K2/K3：候选内核开关

以下是需要维护成 fragment 的意图，**不是本轮已应用的 SDK 补丁**：

```text
# K1，另逐项关闭所有非 AR0234 sensor
# CONFIG_SOUND is not set
# CONFIG_BT is not set
# CONFIG_WLAN is not set
# CONFIG_CFG80211 is not set
# CONFIG_MAC80211 is not set
# CONFIG_RFKILL is not set
# CONFIG_NETFILTER is not set
# CONFIG_BRIDGE is not set
# CONFIG_TUN is not set
# CONFIG_VETH is not set
# CONFIG_PPP is not set
# CONFIG_USB_VIDEO_CLASS is not set

# K2
# CONFIG_ROCKCHIP_RKNPU is not set
# CONFIG_DRM is not set
# CONFIG_FB is not set
# CONFIG_BACKLIGHT_CLASS_DEVICE is not set

# K3，保留原厂启动链的 USB 下载支持
# CONFIG_USB_SUPPORT is not set
```

本轮在 **Distrobox Ubuntu 22.04、SDK GCC 12.4** 下对三个累计候选执行了隔离输出目录的 `olddefconfig`：

| 候选 | y/m 选项数 | 解析结果 |
|---|---:|---|
| 当前 B0 | 1292 | 当前构建的有效配置 |
| K1 音频/无线/网络组 | 1155 | 被请求关闭的功能关闭；核心保留 |
| K2 加显示/NPU | 1116 | DRM/FB/RKNPU 关闭；核心保留 |
| K3 加 USB | 1077 | USB host/gadget 关闭；核心保留 |

核心检查包含 AR0234、CIF/ISP/VPSS、RGA、MPP OSAL、DVBM、STMMAC、DW-MMC、MODULES、PREEMPT/1000 Hz。
这些试验尚未包含逐个 sensor、DTS、OEM 模块和所有用户空间运行依赖，所以不是最终最小配置。
Buildroot 候选同样完成依赖解析，确认 FFmpeg/GStreamer/Python/无线包关闭，ALSA 库、SSH、诊断工具保留。
配置解析成功只能证明 Kconfig 可解；完整编译、modpost 和实机行为尚未验证。
结果在忽略目录 `artifacts/kernel-optimization/kconfig-probes/`。

必须保留 ARM64/SMP、CPU errata 修复、中断/计时器、DMA/IOMMU、DMA-BUF/CMA、PM domains、thermal、watchdog、
CPUFreq/DMC、GPIO/pinctrl/I²C、RK801 regulator、eMMC/ext4、UART、proc/sys/devtmpfs、Unix/TCP/UDP/IP、SSH 所需 crypto/seccomp。
`CONFIG_MODULES=n` 暂不考虑；当前编码路径就是外置模块。
不使用 `localmodconfig` 自动生成最终配置：probe 到但尚未流式运行的驱动、built-in 和双摄路径容易漏掉。
DTS 只逐组关闭无关设备，不改现有传感器寄存器、lane、clock、供电、CSI/ISP 路由。

## 4. 运行时调优：先定位，再固定默认值

### CPU、DDR 和媒体时钟

1. 上板读回 CPU governor、频率和 thermal；保留 performance 作为首选基线。
2. 现有 `S02teleop-performance` 对所有 devfreq 写 performance 并忽略错误，但当前内核没有编译该 governor。
   单独增加 `CONFIG_DEVFREQ_GOV_PERFORMANCE=y`，只对确认的 DMC 节点试验，验证 `available_governors` 和读回结果。
   不再一概锁住闲置 NPU；后续精简镜像会关闭 NPU。
3. 读取双摄运行时 ISP/VENC 的实际时钟、OPP、温度和负载。kmpp 的 `mpp_rkvenc_500.c` 提供 `clk_core` procfs 入口，
   源码默认 core 600 MHz/aclk 300 MHz；实际节点路径与频率先枚举，不直接套用旧文档中的路径写入。
4. 仅在现有合法 OPP 内比较，保留 thermal 限频，不超频或删除温控。
5. CPU idle、`NO_HZ_IDLE` 先保留。只有发现唤醒长尾后，单独比较限制深 idle 的代价；
   不先把 `nohz_full`、`isolcpus`、RCU offload 和实时优先级一起打开。

PREEMPT 已有；PREEMPT_RT 需要当前 vendor 6.1.141 的匹配补丁与驱动审计，暂不作为下一轮默认选项。
可在后期仅改变 HZ=250/1000 做对照，确认增加 tick 是否真正改善 P99，不能默认 1000 一定更快。

### IRQ 与用户线程

先保存 `/proc/interrupts`、`/proc/softirqs`、各进程/线程 CPU 利用率，再比较：

- 默认分配与 sender0/sender1 分别绑核；同核/异核 GMAC、CIF/ISP、VENC IRQ。
- 4 核机器预留一个核承担 SSH/后台，不先给摄像头和网卡所有线程最高优先级。
- 查看 `smp_affinity_list` 是否可写；硬件/managed IRQ 不一定允许移动。
- RPS/RFS 会引入跨核处理，XPS 多队列才可能有实际效果；先查网卡队列数及现有 mask。
- 优先普通调度+亲和性；确有调度迟滞后再试受限的 SCHED_FIFO，保留 RT throttling/恢复能力。

这些是实验变量，不预先写死 CPU 编号。RPS/RFS/XPS 的作用见
[Linux 网络扩展机制](https://docs.kernel.org/networking/scaling.html)。

### UDP/NIC

链路约每路 30 Mbit/s，当前 UDP datagram 最大 1200 字节，双路有效数据远小于千兆链路上限。
维持 MTU 1500，不先启用 jumbo，也不调 TCP 拥塞控制或 `tcp_*` 来优化 UDP。

| 变量 | 首轮试验 | 注意点 |
|---|---|---|
| EEE | 读取两端 EEE，支持时对比关闭 | 避免空闲唤醒代价，记录功耗 |
| IRQ coalescing | 两端读 `ethtool -c`，逐项对比低延迟合法值 | 接收机 RX 是重点；Aura TX 合并主要影响回收/背压，不能一律等同于发送等待 |
| offload | 校验和 offload 先保留；主机 GRO 单独开/关 | 应用未启用 UDP_GRO，不能假定 GRO 当前就合并本流；不一键关闭全部 offload |
| ring | 保持默认，只有积压证据后再缩小 | 小 ring 会增加丢包，不代表更新更快 |
| RPS/NAPI budget | 默认与明确瓶颈下的调整对照 | 盲目增大 `netdev_max_backlog/budget` 会挤占解码/显示时间 |
| socket buffer | 先读有效值和丢包，再比较有限几档 | 大容量不等于一定等待，大积压才增加延迟；小容量也会丢 IDR/slice |
| busy-poll | 后期在主机 socket 试 0/25/50 us | 当前接收用 poll；需应用设置 `SO_BUSY_POLL` 并确认网卡支持；Aura是主要发送端 |
| qdisc | 当前先用现有路径 | 想试 fq/fq_codel 必须先加 NET_SCHED 和对应 SCH 选项，属于另一个内核变更 |

STMMAC 的本地 `stmmac_ethtool.c` 会拒绝 `tx-usecs=0` 和 `tx-frames=0` 同时为零，
RX 某些设置也有驱动限制。因此不提供“一键 ethtool 全清零”的默认值。
先记录设备支持结果、再试参数。参考 [STMMAC 文档](https://docs.kernel.org/6.1/networking/device_drivers/ethernet/stmicro/stmmac.html)。

当前公共 socket 请求 4 MiB 缓冲，既不打印有效值，也忽略 setsockopt 返回值；
它可能受 `net.core.rmem_max/wmem_max` 限制，Linux 读回值又包含内核记账倍数。
下一轮先加可配置 socket buffer、错误/有效值日志、主机 `SO_RXQ_OVFL` 和 socket/UDP drop 计数。
再以 256 KiB、512 KiB、1 MiB、4 MiB 的请求值比较，最终依据 IDR 峰值、调度迟滞和实测丢包确定。
相关 sysctl 行为见 [Linux net sysctl](https://docs.kernel.org/6.1/admin-guide/sysctl/net.html)。

## 5. 编解码与程序路径：预计比继续删驱动更值得测

- 目前双路必须保持 `WRAP_LINES=0`；旧双路 wrap 会串扰/绿块，不能为了低数字恢复。
- VI 使用 3 个 buffer、depth=0、DMABUF；VENC 使用 5 个 stream buffer。分配数量不是固定延迟帧数，
  要看队列是否实际占满。下一轮增加参数，先试 VENC 5/4/3 和 VI 3/2，各组独立检查背压/破帧。
- 当前 H.264 High、CBR、GOP 15、slice 120。检查实际码流无 B 帧、无重排序；
  再试 slice 60/120/240、GOP 15/30、码率 15/20/30 Mbit/s，保持清晰度和丢包恢复。
  不为了测试暂时切 H.265；千兆直连当前不是码率瓶颈。
- **slice 已启用，但目前不是 slice 到网络的贯通流水**：`mpp_sender.cpp` 以 PTS 聚合完整 AU，
  等下一 PTS 才确认上一帧完整并发送，以避开 SDK 不可靠的 `bFrameEnd`。
  应记录“第一 slice、最后 slice、下一 PTS、UDP 排队”时间，验证这个等待是否贡献约一个采集周期。
  不能简单把 slice 当完整帧或提前发送残帧；此前这种错误已经造成解码损坏。
- 主机先完整 UDP 重组，再喂 Annex-B parser。parser 也可能等下一 AU 边界。
  需要测 packet → decoded frame，而不是按名义 `low-delay` 推断无等待。
- 主机接收、解析、VAAPI 解码和 `present_stream` 在一个循环中串行运行；
  某一路显示提交阻塞就可能拖住两路 UDP 接收。先分段计时，再考虑独立收包、每路有界解码队列和独立显示调度。
- 每路重组器只有一个在建帧，但完整帧仍可能在 socket 积压，所以现有代码并非所有阶段的 newest-frame-wins。
  H.264 参考 P 帧不能随便丢掉；应继续维持解码参考链，只丢旧的已解码显示帧，或设计明确的丢包/IDR 恢复策略。

这些应用优化与内核裁剪分开提交、分开 A/B，不改变 B0 程序二进制来测“纯内核收益”。
两路没有硬件曝光同步，双目时间偏差是另一项指标，不和各路传输延迟混为一谈。

## 6. GCC / Clang 的可行路线

| 试验 | 当前支持程度 | 推荐顺序 |
|---|---|---|
| GCC 内核 O2 | 已实际使用 | 保留主线 |
| GCC 内核 `-mtune=cortex-a53` | 编译器已验证接受；当前内核命令没有该选项 | 单独试，保留内核所有语义/errata/通用寄存器限制 |
| 内核全局 O3 | 当前 Kconfig 只有 O2/Os，没有正式 O3 选项 | 后期实验，KCFLAGS/源码改动必须核对每个实际命令和模块 |
| GCC 内核 LTO | 此 vendor Kconfig 无 GCC LTO 路径 | 不直接塞 `-flto`；需要专门构建系统移植，优先级低 |
| Clang O2，无 LTO | ARM64/Kbuild 支持；本 SDK 尚未完整构建验证 | 先独立编译+启动+媒体模块验证 |
| Clang ThinLTO | ARM64 支持 `LTO_CLANG_THIN`，需 Clang/LLD/LLVM tools/IAS | 在 Clang 无 LTO通过后对照 |
| Clang Full LTO | 有 Kconfig，链接时间/内存较大 | 后于 ThinLTO，不能假设更低延迟 |
| 内核 PGO/AutoFDO | 未发现本树现成 PGO/AutoFDO/Propeller 配置入口 | 不当成只开一个选项就能用的方案 |
| GCOV | 有覆盖率基础设施，非自动 PGO 流程 | 仅调试/取样，不放入延迟验收镜像 |
| 自有程序 IPO/LTO | GCC AArch64 小程序 `-O3 -flto` 交叉链接已通过 | CMake `CheckIPOSupported`，先仅用于自有源码 |
| Buildroot O2/O3/LTO | 存在 `BR2_OPTIMIZE_2/3`、`BR2_ENABLE_LTO` | O2先对照；全局O3/LTO后于热点分析 |

官方 [GCC 12.4 优化说明](https://gcc.gnu.org/onlinedocs/gcc-12.4.0/gcc/Optimize-Options.html)
没有保证 O3 比 O2 更快；代码膨胀也可能损害指令缓存。
`-mtune` 改变调度/成本模型，和新增指令集的 `-march` 不同，见
[AArch64 选项](https://gcc.gnu.org/onlinedocs/gcc-12.4.0/gcc/AArch64-Options.html)。

内核实际命令包含 `-mgeneral-regs-only`、`-fno-strict-aliasing`、`-fno-strict-overflow`、frame pointer 和 A53 errata 支持；
这些必须保留，不使用 `-march=native`、`-Ofast`、fast-math 或通过移除 ARM errata 修复追求性能。
当前汇编器的 `-Wa,-march=armv8.5-a` 服务于内核汇编能力，不表示整个内核 C 代码要求板子执行 ARMv8.5 指令。

本轮已在 Distrobox 验证 GCC 接受 `-O2 -mgeneral-regs-only -mtune=cortex-a53`，
归档 GCC O2/O3 实际 optimizer 列表，并用两个 C 翻译单元成功交叉链接 AArch64 LTO 示例。
这仅证明工具链选项/链接能力，**没有证明内核 O3/LTO 或目标程序运行性能**。
sender/viewer 目前已是 O3；新增 IPO 不能跨越现有 `librockit.so`/MPP 动态库，也不能重优化发行版 `.S`。

Clang 的完整工具只在 CachyOS 宿主机能找到；此次 `aura-sdk` Distrobox 内没有找到完整 LLVM 工具集。
后续进入该轮再请用户安装；仍在 Ubuntu 22.04 中做板端构建，不能为用现成 Clang 改到宿主机编译 SDK。
先用隔离 `O=` 目录验证 `make ARCH=arm64 LLVM=1`，然后移植 SDK wrapper、kmpp 与 Rockit 模块构建。
配置和构建都用同一 LLVM 设置，不混用旧 GCC `.o`、`.ko` 和 `Module.symvers`。
Kbuild 的 LLVM 使用方式见 [Linux 6.1 LLVM 文档](https://docs.kernel.org/6.1/kbuild/llvm.html)，
ThinLTO 机制见 [Clang ThinLTO](https://clang.llvm.org/docs/ThinLTO.html)。
重建外置模块要匹配新配置/符号，单靠 vermagic 相同不构成兼容性证明，见
[外置模块构建](https://docs.kernel.org/6.1/kbuild/modules.html)。

## 7. 每阶段交付与停止条件

每阶段记录：源码提交/patch、有效 config 差异、镜像 SHA-256、完整构建日志、模块加载与 ELF 依赖检查，
以及 [双路延迟基线计划](stereo-latency-baseline-plan.md) 中的功能、稳定性和光学结果。
板端构建/配置检查用 Distrobox，host viewer 用 CachyOS。每阶段使用本机 Git 身份提交，生成物继续 ignore。

先检查 core/media pipeline、双路图案不串扰、SSH/静态网络，再测延迟；出现 hang、CSI 错误、花屏、持续丢帧、
模块 undefined symbol 或 P99 明显退化就回退该阶段。无实测收益的调参不并入默认配置。
精简成功可以保留体积收益，即使光学均值没有下降；不得写成虚构的毫秒收益。
需要刷写时给出对应镜像和校验值，由用户操作，不能自动烧录或重启。
