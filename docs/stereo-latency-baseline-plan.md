# 双 AR0234 未裁剪镜像的延迟基线与 A/B 测试

2026-10-12。与 [内核/Buildroot 优化计划](kernel-buildroot-optimization-plan.md) 配套。
现有 [screen-to-screen 测试图方案](screen-to-screen-latency-test-plan.md) 解释了 `latency-stimulus`；
本文补充双路 AR0234 的冻结条件、测量误差、统计方式和每次裁剪的验收顺序。

## 1. B0 的准确含义与保存位置

B0 是**当前未进行进一步裁剪的双 AR0234 镜像**，不是原厂 defconfig。
它已经有 PREEMPT、1000 Hz、内核 O2、CMA 256M、AR0234 移植及静态 IP/SSH。
现有单目 MIA1321 的 20～30 ms 是历史观察，不能直接当作双 AR0234、关闭 wrap 后的基线。
本轮没有拍摄光学录像；双路 B0 的 P50/P95/P99 尚待测量。

本轮已保存以下内容到本机忽略目录：

```text
artifacts/kernel-optimization/baseline-20261012/
  manifest.json
  kernel.config / buildroot.config
  BoardConfig-...-AR0234-Stereo-Buildroot.mk
  env.img / idblock.img / uboot.img / boot.img
  userdata.img / oem.img / rootfs.img
  teleop-sender / host-viewer / latency-stimulus
```

这些文件是独立的复制/写时复制备份，不是指向 `output/image` 的软链接。
备份用了当前工作区 b4bd5e7 的源码和现有二进制。
可跟随 Git 分享的元数据在 [审计快照](baselines/2026-10-12-sdk.json)；大镜像、captures 和日志不进 Git。
回退也由用户按原分区布局刷写，完整刷写会覆盖 `/userdata`。

主要分区 SHA-256：

| 文件 | SHA-256 |
|---|---|
| boot.img | `57b4c2ae0654ab1eb7128f7faa3f0d944826ef0d2e468da325861d8fcc9dd3d5` |
| oem.img | `181d86d22229ca7fe239590e4b92845217bf04ef277d1c421f734768eb71f03d` |
| rootfs.img | `f9f4270edc908031c739d247a0533883f8c92cf82310fe935bdadfb8271ffaa0` |

不要在第一次 baseline 前顺手重编 sender/viewer、改 governor 或升级主机驱动。
若板端二进制与快照 hash 不一致，先记录实际版本，选择同一套已验证程序固定使用；
镜像 hash 是本机文件的标识，本次 SSH 连接被关闭，没有重新读取板端固件或程序来确认一致性。

## 2. 固定实验条件

| 条件 | B0 设置 |
|---|---|
| 采集 | 两颗 AR0234，1280×720 NV12，实际约 99.23 fps |
| 传感器控制 | 两路 exposure=735、analogue_gain=32、digital_gain=512、test_pattern=0 |
| 编码 | H.264，目标 100 fps，30,000 kbit/s/路，GOP 15，slice 120 |
| wrap | 两路均 0，保留当前已验证完整帧路径 |
| 启动 | 现有 `run-ar0234-stereo`，先等 camera0 ready，再初始化 camera1 |
| 网络 | Aura eth0=192.168.1.10，host enp195s0=192.168.1.2；直连 1Gbps full duplex，MTU 1500 |
| 解码/显示 | host FFmpeg VAAPI → DRM PRIME → Vulkan/SDL3，`--dual-stream` |
| 屏幕 | 当前旋转后的 144 Hz 副屏，上下布局；保持相同旋转、缩放、窗口位置与尺寸 |
| 摄像头/光照 | 固定支架、拍摄范围、焦点、照明；锁住曝光与增益 |
| CPU/GPU/合成器 | 记录现状；B0不改变，不在内核对比期间切 Wayland/X11、刷新率或GPU模式 |
| 供电/散热 | 同一电源/线材/散热；热稳态后测量，记录温度/降频 |

exposure=735 接近一帧周期，只是当前可见且已验证的工作值；缩短曝光可能改善某些光学响应，
但会改变噪声/亮度和测量条件，必须作为单独实验，不能与内核裁剪一起修改。
默认启动时出现的 vblank 警告/初始帧跳变单独统计，进入稳定段后不应持续出错。

## 3. 三类结果分开报告

1. **光学 screen-to-screen**：刺激屏实际显示变化，经传感器、编码、网络、解码，直到返回屏实际显示。
2. **板端/主机内部耗时**：同一台机器上的 monotonic 时钟测出的阶段时间，用于定位瓶颈。
3. **帧率/丢帧/稳定性**：采集间隔、码流/重组/解码/显示计数、异常恢复、双目画面的时间差。

不使用未同步的板端 PTS 减主机时钟作为单向延迟。`--same-clock` 只适合同主机 loopback。
TCP ping/iperf 可用于确认链路，不代替光学视频延迟。
双摄无硬件同步，分别报告两路延迟与 skew；viewer 的异步上下画面不保证同一次曝光。

## 4. 执行顺序

### A. 版本与状态快照（不调整系统）

记录 host、板端系统/程序 hash、`/proc/cmdline`、有效内核配置、传感器 controls、media graph、模块列表、
当前后台进程、两端 NIC 参数和路由。测试前后都记录 UDP/NIC/softnet 计数，**计算差值，不清空 dmesg 或重置计数**。

板端可通过 SSH 执行以下只读检查，输出存到 `logs/latency/<run>/board-before.txt`：

```sh
uname -a
cat /proc/cmdline
sha256sum /userdata/teleop-sender /userdata/run-ar0234-stereo
cat /proc/meminfo
cat /proc/interrupts
cat /proc/softirqs
cat /proc/net/softnet_stat
cat /proc/net/snmp
ps
lsmod
ethtool eth0
ethtool -k eth0
ethtool -c eth0
ethtool -g eth0
ethtool --show-eee eth0
ethtool -S eth0
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    echo "$p"
    cat "$p/scaling_governor" "$p/scaling_cur_freq" "$p/scaling_available_frequencies"
done
for p in /sys/class/devfreq/*; do
    echo "$p"
    cat "$p/governor" "$p/available_governors" "$p/cur_freq" "$p/available_frequencies" 2>/dev/null
done
for p in /sys/class/thermal/thermal_zone*/temp; do echo "$p"; cat "$p"; done
```

不支持的 ethtool/sysfs 项记为 unavailable，不能使整套快照中止。
记录 `net.core.rmem_max/wmem_max` 和 `ss -u -a -m -p`；上面清单可在取得连接后收集，本轮没有冒充已收集。
主机按相同方式检查 `enp195s0`，另记录桌面实际显示模式、FFmpeg/VAAPI/SDL3/Vulkan 和 GPU 驱动版本。
深度 tracing/perf 取样放在单独诊断 run，正式光学 run 避免取样开销混入。

### B. 正确性与启动稳定性

先做两路静态彩条/不同图案确认，随后恢复真实画面；必须验证两路独立、完整宽度、无绿块和条纹。
再用 `/userdata/run-ar0234-stereo --capture-only --frames 600` 检查原始采集。
不要同时运行 v4l2-ctl capture 与 sender，也不强行启动第二个 launcher。

运行有限帧编码/接收测试。先在 CachyOS 启动 viewer，确认窗口和端口 ready：

```sh
mkdir -p logs/latency/B0-smoke
./build/host/bin/host-viewer \
  --port 5000 --dual-stream --frames 12000 --timeout-ms 20000 \
  --window-width 1080 --window-height 1216 --borderless \
  >logs/latency/B0-smoke/viewer.txt 2>&1
```

再从另一终端通过 SSH 启动板端，每路 6000 帧、约 60 秒：

```sh
ssh -t root@192.168.1.10 \
  'REPORT_DIR=/userdata/latency-B0-smoke WRAP_LINES=0 /userdata/run-ar0234-stereo --frames 6000'
```

当前 viewer 的 `--frames` 是**两路完成 AU 的总数**。理论目标是总 AU 12000、两路各解码 6000、
`hardware_frames==frames`，零 malformed/duplicate/dropped、无 FFmpeg 解码错误，两路真实画面独立。
presenter 的计数是提交次数，不是每路面板实际刷新次数；解码率约 200 帧/秒也不能叫屏幕 200 Hz。
未达到目标时先保存日志、解释丢失/解析尾帧与启动时刻，再决定是否进入延迟测试。

随后先做 10 分钟并反复启动/停止 10 次，最后做至少 1 小时稳定性测试。
有过一次未经解释的 SDK/内核 hang，因此“12 秒通过”不能代替长测。
重启/失联、出现花屏或持续丢帧的配置标为不合格，不用较低平均延迟掩盖稳定性问题。
不在长测同时写 H.264 到 eMMC 或运行 saturating iperf；需要保存码流时另开短诊断 run。

### C. 光学 B0

在旋转副屏上采用三个观察区域：顶部 `latency-stimulus`，下方 viewer 的 camera0、camera1。
保持用户现有上下布局，不改成左右排布。两个镜头都只拍刺激区，不能把返回画面再拍回造成反馈。
窗口几何按实际桌面可用高度安排；上面的 1080×1216 是现有双路 viewer 示例，总高度包含两路。

运行连续版本：

```sh
./build/host/bin/latency-stimulus \
  --width 1080 --height 700 --borderless --vsync on

./build/host/bin/host-viewer \
  --port 5000 --dual-stream --frames 0 --timeout-ms 3600000 \
  --window-width 1080 --window-height 1216 --borderless

# 单独的 SSH 终端
ssh -t root@192.168.1.10 /userdata/run-ar0234-stereo
```

KWin Wayland 的实际位置由平铺/手动安排；记录几何，不把 `--window-x/y` 当作可靠定位。
刺激窗口顶上的数字是该程序的显示提交序号，跟随实际 present 速率，
不是 AR0234 的 100 fps 帧号；先从刺激日志确认它确实接近 144 fps。

固定刷新率/VRR 状态、缩放、亮度，优先用外部 240 fps 或更高速的相机同时拍到三个区域。
锁定外部相机曝光、焦点和白平衡，曝光尽量短到能读清一次刷新，避免两个时间码叠在一起。
不使用 KDE 截图、桌面录屏或 viewer 截帧替代外部录像：这些不能测到面板真正发光。

每组先热稳态至少 2 分钟，拍 3 段各 30～60 秒；另外保留冷启动录像/首次出画时间作为独立指标。
对外部录像同一帧读取：

```text
d0 = 顶部当前毫秒码 - camera0 返回图里的毫秒码
d1 = 顶部当前毫秒码 - camera1 返回图里的毫秒码
skew_code = 两路返回毫秒码的差
```

`skew_code` 是显示内容差，混合了异步曝光/传输/显示，不宣称它是硬件曝光同步误差。
重复返回图也要记录，不能只挑最清晰、最新或最小延迟的帧。
读数跨时码周期时正确回绕；Gray code/十进制矛盾、显示正在切换或数字不可读的样本标为无效并计数。

### D. 测量误差与判读

144 Hz 面板每次刷新约 6.94 ms，99.23 fps 采集周期约 10.08 ms；
240 fps 外部相机采样间隔约 4.17 ms，曝光/rolling shutter 还会引入不确定性。
毫秒数字的分辨率并不等于测量准确度 1 ms。
软件提交时间码和面板发光有偏差，因此这种上下读码首先是可重复的光学比较方法；
未经显示扫描/提交滞后校准，不宣称获得亚毫秒绝对 screen-to-screen 真值。

旋转屏幕的**逻辑上下位置不一定是物理扫描的上下位置**，扫描可能沿逻辑水平方向。
分别记录三个 ROI 的物理位置，用同一整屏同时变化的标记和高速录像检查扫描/外部相机 rolling shutter 的相位差。
如果没有可靠校准，就同时报告原始读码结果和这些误差源，不随意减去“半屏=3.47 ms”。
若另实现本地直接复制校准，要将其渲染排队代价与物理扫描偏差分开，不能把所有本地复制延迟都当面板常数扣除。
需要确认小于一个刷新周期的收益时，改用已校准光电探头/示波器的亮度跳变测量。

## 5. 统计与记录格式

每路独立报告 N、P50/P95/P99、min/max、均值、无效读数、重复/丢帧、帧率、温度和异常恢复。
P99 至少取 1000 个可读样本/路，建议数千个；手工只读几十帧时只能作为初步观察。
按固定规则取样，避免按屏幕刷新/采集周期固定步长造成混叠；自动读码工具后续实现，当前尚未存在。
连续样本有关联，置信度比较按录像片段或时间块处理，不能把每个重复画面视为独立观测。

后续结果表建议保存到 `captures/latency/<run>/samples.csv`：

```text
run_id,variant,stream_id,reference_frame,source_code_ms,returned_code_ms,raw_delay_ms,valid,invalid_reason
```

另写 `metadata.json`：image/program/config hashes、所有命令/参数、镜头/ROI 几何、显示模式、
外部相机 fps/曝光、板端与主机 governor/NIC 参数、热稳态温度、是否有相位校正及依据。
录像/CSV/日志放 ignore 目录；结论、参数和提交信息写进 docs。

| variant | 改动 | camera0 P50/P95/P99 | camera1 P50/P95/P99 | 丢帧/花屏 | 温度/CPU | 长测 |
|---|---|---|---|---|---|---|
| B0 | 当前镜像 | 待测 | 待测 | 待测 | 待测 | 待测 |
| S1 | 仅用户空间裁剪 | 待测 | 待测 | 待测 | 待测 | 待测 |
| K1 | 音频/无线/网络裁剪 | 待测 | 待测 | 待测 | 待测 | 待测 |
| K2 | 显示/NPU裁剪 | 待测 | 待测 | 待测 | 待测 | 待测 |
| K3 | Linux USB裁剪 | 待测 | 待测 | 待测 | 待测 | 待测 |
| N1-* | 单项运行时调优 | 待测 | 待测 | 待测 | 待测 | 待测 |
| C1-* | 单项编译器试验 | 待测 | 待测 | 待测 | 待测 | 待测 |

## 6. A/B 与分段定位

每次只改变一组裁剪或一个调参，保持程序、曝光、图像尺寸、码率和显示设置一致。
先按 B0→候选→B0 复测，后续可交错 A/B/A/B，控制散热/时间漂移。
候选全量镜像中的 boot/OEM/modules 必须配套，不能只刷 boot 造成模块 ABI 错配。
记录每一版镜像的实际选项差异；并非每阶段都必须降延迟，体积/开机收益另报。

当前尚未实现的分段计时下一轮加到程序里：

| 同机区间 | 记录点 | 能回答的问题 |
|---|---|---|
| 板端编码/AU 等待 | 第一 slice、最后 slice、下个 PTS、sendmmsg 前后 | 是否额外等待一帧；网络排队还是编码慢 |
| 主机收包 | 首包/末包/重组完成、RX overflow、socket backlog | 网络抖动/用户线程被显示阻塞 |
| 解码 | parser/packet 送入、VAAPI frame 输出 | parser 是否等待下一帧、解码队列是否增长 |
| 显示 | frame 可用、Vulkan 提交、present 返回 | CPU侧排队；present 返回仍不等于光子发出 |
| 可选跨机 | 经验证的 PTP 同步 + 硬件时间戳 | 只在明确同步误差后估计单向网络耗时 |

避免逐包 printf；用内存缓冲/汇总直方图，深度日志单独跑。
降低 rockit/内核刷屏日志也单独对照，但保留错误采集；不能靠隐藏 FFmpeg 错误来通过验收。
吞吐压力、CPU 背景压力和真实光学稳态分别运行；先通过无负载稳定性，再测 P99 对压力的敏感性。

接受优化的条件是无正确性/稳定性回归、各路帧率不退化，且目标指标改善超过测量与 run 间波动。
例如固定一套方法观察至少约 5 ms 的 P95/P99 改善，再通过重复 A/B确认；
小于当前测量不确定性的差异记为“未分辨”，不用单次最小值声称突破。
下一步优先完成 B0 光学与长时测试，再进入 S1；需要刷写由用户执行。
