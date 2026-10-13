# 调度与电源策略 A/B：实现与实机记录

本轮完成六类策略对照。**默认保留现有调度和电源策略**：没有发现足够一致的收益来
支持把所有性能开关长期打开。新增参数均需显式指定，测试后系统配置已恢复。
只更新本机 viewer 和测试工具，没有修改 SDK、sender 或镜像，无需烧录。

## 环境与对照方法

本机 Ryzen AI 9 HX 470、24 个逻辑 CPU，0–3/12–15 为最高频率约 5.3 GHz 的物理核/SMT 对，
其余最高频率约 3.35 GHz。NIC IRQ 在 CPU5，GPU IRQ 在 CPU23。
`amd-pstate-epp` active，balanced 档为 powersave governor + balance_performance EPP；
GPU power level 为 auto，sched_ext disabled，ananicy-cpp 在运行。
启动 nice 值在不同组中为 -4 或 -8；每一组 A/B 内保持相同，不跨组比较绝对值。

板端原 A 内核 `6.1.141 #16 SMP PREEMPT`，boot ID 始终为
`32d05d60-7683-4865-868e-866f1a7217b9`。VENC 为 venc_ondemand，396/550 MHz 两档，
测试中读取 cur_freq 已为 550 MHz。板端没有可用 CPU cpufreq policy，NPU 未参与链路。
两颗 AR0234、1280×720、约 99.23 fps、30 Mbps、GOP15、mode1、wrap=0；viewer 批量上限 16。

每组交替 A/B 三次，再追加 A-return；本机电源和最终 viewer 唤醒约束另有真实窗口 A/B。
每次每相机 120 帧，启动温度阈值 70℃，先验证独立 USB `/dev/kmsg` live marker。
每次检查配置生效、退出/解码/队列统计，并恢复配置；只比较单个策略变量。

| 策略 | A | B |
|---|---|---|
| 本机电源档位 | balanced | powerprofilesctl 临时 performance hold |
| 板端 VENC 下限 | min=396 MHz，max=550 MHz | min=max=550 MHz，保留原 governor |
| RX 绑核 | 原允许集 0–23 | RX 只在 CPU1，其他线程不指定 |
| RX 实时策略 | RX 已绑 CPU1，SCHED_OTHER | 同样绑 CPU1，RX SCHED_RR priority10 |
| 板端 GMAC IRQ | IRQ71 allowed=0–3，effective=0 | IRQ71 allowed/effective=1 |
| CPU idle 唤醒约束 | 不新增 PM QoS 请求 | /dev/cpu_dma_latency 请求 50 μs |

本机 performance hold 使 governor/EPP 都成为 performance，GPU 仍为 auto；
它向硬件提供性能偏好，频率仍受硬件、电源和温度限制。
[amd-pstate 文档](https://docs.kernel.org/admin-guide/pm/amd-pstate.html)

CPU1 是本机一颗较快物理核，此处绑核不代表隔离或独占 CPU。
RR 只作用于 RX worker，其他线程保留继承策略；默认不设实时优先级。
[Linux 调度策略](https://man7.org/linux/man-pages/man7/sched.7.html)

PM QoS 是全局 CPU idle 延迟约束，请求按最严格值聚合，关闭 fd 就删除本进程请求。
本机 idle state 标称退出延迟为 0/1/18/300 μs，50 μs 候选允许较浅空闲态。
它可能增加空闲功耗；50 μs 不约束整个网络、调度或 GPU 链路的完成时间。
[PM QoS 文档](https://docs.kernel.org/power/pm_qos_interface.html)

## 数据与决定

以下是三次 headless 的样本加权均值，格式均为 A→B。
各组背景状态不同，横向绝对值不代表累计优化量。

| 策略 | 内核 RX→应用 μs | stream0 AU完成→FFmpeg返回帧 ms | stream1 同阶段 ms | 决定 |
|---|---:|---:|---:|---|
| performance 档位 | 7.59→6.43 | 0.175→0.181 | 0.207→0.268 | 收包略改善，解码队列无一致改善，不默认启用 |
| VENC min=550 MHz | 7.79→7.35 | 0.165→0.165 | 0.192→0.195 | 原本已在高档，未证明有效收益，恢复 min396 |
| RX CPU1 | 8.65→8.86 | 0.179→0.142 | 0.170→0.172 | 降低迁移，但 RX 等待/另一流没有一致改善，保留可选 |
| RX RR10 | 8.50→8.75 | 0.139→0.153 | 0.165→0.174 | 未见收益，不默认开启 |
| GMAC IRQ71 CPU1 | 7.21→7.25 | 0.172→0.156 | 0.203→0.182 | 有少量改善趋势，仍是短样本，恢复原路由 |
| idle50，由测试控制器持有 | 7.71→7.36 | 0.192→0.182 | 0.193→0.183 | 尾部等待改善，继续验证实际应用生命周期 |
| idle50，由最终 viewer 持有 | 7.48→8.39 | 0.161→0.164 | 0.189→0.182 | 收益缩小且不一致，作为实验选项 |

RX 绑核组的 RX 迁移由每次 490–1599 次降到 1–2 次；这是亲和性成功生效的证据，
迁移下降不能单独当成延迟下降。decoder 没有绑核，仍可迁移。
GMAC 的 stream0/1 UDP 入队调用均值分别从约 0.312/0.403 ms 到 0.301/0.394 ms，
stream0 每轮 p99 范围从 0.696–0.795 ms 到 0.664–0.680 ms；改善量较小，尚无持续负载验证。

idle50 最初控制器持有时，stream0 队列等待 p99 每轮范围由 52–303 μs 到 34–48 μs，
stream1 由 26–252 μs 到 26–42 μs。但 A-return 本身也恢复到较低等待，提示背景噪声。
最终应用自己持有请求的复测：

| 指标 | A | B idle50 |
|---|---:|---:|
| stream0 队列等待均值 μs | 13.24 | 19.24 |
| stream1 队列等待均值 μs | 14.63 | 19.24 |
| stream0 AU→返回帧 p99，各轮范围 ms | 0.344–0.655 | 0.328–0.356 |
| stream1 同阶段 p99，各轮范围 ms | 0.365–0.590 | 0.418–0.500 |
| 窗口显示调用均值 ms，单次 | 2.223 | 2.237 |
| 窗口显示调用最大值 ms，单次 | 88.57 | 87.84 |

不能把首组较好的尾部数据直接作为稳定收益承诺。当前支持用户按实际负载选择 idle50，
不自动改变默认值，也不叠加 RR、绑核、performance 或 VENC/IRQ 调整。

VAAPI 返回帧是软件可见对象时间，显示调用返回不保证 GPU 完成或屏幕出光。
软件 RX 时间戳也不覆盖网卡中断发生前的所有唤醒等待；本轮未测一端到另一端或 screen-to-screen。
每轮只有 120 个帧样本，p99 是现有直方图的桶上界及各轮范围，没有计算虚假的合并分位数。

## 新参数与复测

viewer 原启动命令继续可用。需要试验会话级唤醒约束时：

```sh
cd /home/ww/codings/luckfox-aura-teleop
TELEOP_METRICS=1 ./build/host/bin/host-viewer \
  --dual-stream --frames 0 --timeout-ms 3600000 \
  --window-width 1080 --window-height 960 --borderless \
  --cpu-latency-us 50 --queue-report-ms 1000
```

运行时输出 `cpu-latency: requested_us=50 effective_us=50`，正常退出或异常退出释放请求。
支持 0..10000 μs，默认不打开节点；本机通过 audio 组访问，无需 sudo。
其他机器权限不足时明确报错，不通过伪装设备或静默降级掩盖失败。

调度参数：`--rx-cpu N`、`--decode0-cpu N`、`--decode1-cpu N`、`--rx-rr-priority N`。
CPU 必须属于线程继承的允许集；RR 0 保留继承策略，1..99 显式设置 RR，受系统权限/rlimit 控制。
线程输出 `thread-policy`：实际 affinity、policy、priority、nice、当前 CPU；
退出输出 `thread-run`：用户/内核 CPU 时间、主动/被动切换和迁移计数。
这些参数不修改 SDL 主线程、系统 IRQ 或所有进程的调度。
decode 绑核参数已通过原生 VAAPI 回归，本轮没有对其做额外实机收益评价。

复测工具：

```sh
./tools/test-scheduling-power-ab --experiment host-power --display-pair --output logs/policy/new-power
./tools/test-scheduling-power-ab --experiment venc-floor --output logs/policy/new-venc
./tools/test-scheduling-power-ab --experiment rx-affinity --output logs/policy/new-affinity
./tools/test-scheduling-power-ab --experiment rx-rr --output logs/policy/new-rr
./tools/test-scheduling-power-ab --experiment gmac-irq --output logs/policy/new-gmac
./tools/test-scheduling-power-ab --experiment host-idle --display-pair --output logs/policy/new-idle
```

默认 RX CPU1/RR10/IRQ71→CPU1 是本机实验配置，其他机器需传正确编号。
`--rounds 1` 缩短为一对加 A-return；`--idle-owner controller` 复现初始控制器持有方式。
SSH 凭据沿用既有配置，可使用 SSHPASS；工具不会烧录或重启。
恢复 GMAC 时先设置原 effective CPU，再恢复原宽 mask，避免 A-return 实际仍在 B CPU。
远程恢复优先 SSH，失败时尝试独立 ADB；恢复失败就停止后续测试。
`test-aura-software-baseline` 可单独传 viewer 调度/QoS 参数，记录本机策略和温度快照。

## 验收与恢复

总计 **59 次短测 / 14160 硬解帧**：传输完整、两路各120帧，零 malformed/重组丢帧/
RX溢出/编码队列溢出；USB marker 后没有 BUG/WARNING/软锁/硬锁，boot ID 未变。
收尾发现旧日志镜像 tail 可在 SSH 退出后残留。已清理，并让工具记录其 PID、
将 /proc/PID/cmdline 的 NUL 转为空格后核对唯一日志路径，再终止自己的镜像进程。
补做 6 次短测，最终三次确认没有残留 reader。旧数据作为候选探索，不当作严密隔离下的微秒收益证明。
原生零警告构建；20 项 viewer、2 项已有 VAAPI、3 项 CTest、6 项策略回收测试通过。
覆盖绑核、RR权限失败、PM QoS正常/错误退出，VENC异常回收、IRQ effective恢复、
PM QoS fd异常关闭及无A/B差异时拒绝测试。

最终本机 balanced，24 个CPU的 governor/EPP/min/max 与初始审计一致，GPU auto，无本轮 profile hold；
PM QoS aggregate 恢复到默认 2000000000 μs，viewer 已退出。
板端 venc_ondemand、min396/max550 MHz、IRQ71 mask0–3/effective0，最终约43.69℃，sender已退出。
GPU强制高频未测试，本轮没有修改NPU、CPU idle驱动、系统调度器或新增永久开机策略。

机器摘要在 `docs/baselines/2026-10-13-scheduling-power.json`；原始日志在忽略目录
`artifacts/scheduling-power-20261013/`。前六组使用同一较早二进制，最终 viewer QoS组使用新增
会话参数后的二进制；每一组内部A/B二进制相同，摘要分别保存哈希。
下一步更值得单独检查首帧渲染的缓存/预热；约88ms首帧显示尖峰没有被这些调度策略消除。
