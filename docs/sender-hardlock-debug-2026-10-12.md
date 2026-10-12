# 双 AR0234：CPU0 hardlock 排查与下一版诊断镜像

#16 后续实机结果见 [运行记录](sender-debug16-runtime-2026-10-12.md)：无netconsole
整套生命周期及10次双路短测通过；verbose netconsole对照在单路SYS_Exit阶段失联。
下文保留构建交付时的证据和计划。

目前仍未解决重复双路启动导致整板失联。#14 诊断内核已实测；这轮抓到了 CPU0
停在 `queued_spin_lock_slowpath` 的硬件 PC，尚缺具体锁地址和调用者。
准备了 #16 诊断镜像，等待用户烧写；没有自动烧录或重启。
本轮没有测 screen-to-screen，也没有把未同步的板端/本机时钟相减。

## 本轮测量条件

- 板端内核：6.1.141，#14 SMP PREEMPT，2026-10-12 18:51:27 CST。
- 两路 AR0234，1280×720，VBLANK=21，约99.23fps；单进程 `--dual`、独立 VENC/UDP 线程。
- 发送程序沿用上一轮 MPI 生命周期修复，SHA256 为
  `36468d1e7192db7274718b181fb20d631ee27fbeed367d9c96f0f1f706cf07aa`。
- 每个正常采集用例每路120帧；故障注入和信号用例有外层超时，失联后停止测试，等待用户重启。
- 按用户要求，将测试工具启动温度上限从50°C改为70°C；生命周期预检预留0.5°C裕量。
  这是测试启动门槛，不是修改内核 thermal trip，也没有关闭芯片原有温控。

修复了 `/proc/<pid>/comm` 在读取前进程退出造成的采样竞态：忽略该项读取错误，
温度通过专用 `temperature=` 行解析。测试工具不再把错误文本当作温度整数。

## 实机证据

| 条件 | 结果 | 可得结论 |
| --- | --- | --- |
| netconsole 开启，正常双路/单路 | 多个120帧用例通过，自测内核日志包收到 | 日志通道实际可用 |
| netconsole 开启，故障注入准备阶段 | camera0 VI_EnableChn 内失联；尚未达到注入点 | 初始化阶段也会卡死 |
| 重启后不创建 netconsole 目标，冷启双路 | VI/VENC/bind 就绪后失联 | 原问题不依赖 netconsole |
| 提前设置 watchdog_thresh=3，eth0 IRQ71/72 亲和性=CPU3，不开 netconsole | 整套生命周期测试通过 | 该次正常/回滚/退出路径均可恢复 |
| 同样条件，额外10次双路重启短测 | 前9次通过，第10次硬锁、SSH退出255 | IRQ迁移没有修复根因 |

整套通过的用例包括 dual-start、mono0、mono1、dual-after-mono，三个故障注入点
`camera1.vi.enabled` / `camera1.venc.created` / `camera0.stream.acquired`，
TERM-during-init、INT-after-ready（含进程锁拒绝并发实例）、dual-recovery。
故障注入预期退出1，正常/信号测试退出0；正常出流用例接收并硬件解码完整帧，UDP错误为0。
成功用例资源清理返回0，CMA读数回到测试前水平。这些结果不能覆盖随后复现的内核硬锁。

第10次重复测试的关键内核证据（原始 `/dev/kmsg` 格式）：

```text
CPU0 online:1
PC: <0xffffffc008b1a264> queued_spin_lock_slowpath+0x54/0x2e0
CPU1 online:1
PC: <0xffffffc008b11638> cpu_do_idle+0x48/0x74
CPU2 online:1
PC: <0xffffffc008b11638> cpu_do_idle+0x48/0x74
CPU3 online:1
PC: <0xffffffc00849f76c> rockchip_panic_notify+0x228/0x3b0
Watchdog detected hard LOCKUP on cpu 0
```

这里 CPU0 的 PC 来自 Rockchip 硬件调试采样；CPU3 的 watchdog/notify 调用栈是
报告者的栈，不是 CPU0 获取锁的完整调用栈。网卡 IRQ 迁到 CPU3 后，SSH 多活了一段时间，
保留了这份日志；没有据此把故障归因于以太网、摄像头驱动或某个 MPP 调用。
温度问题仍未完全排除，但已有明确的内核锁等待证据，后续优先定位这把锁。

另一份 netconsole 对照日志的 soft lockup PC 是 `net_rx_action+0x174/0x3a4`，
匹配 #14 vmlinux 的反汇编落在内联 `netpoll_poll_lock` 的原子重试附近。
该次可能受日志通道自身竞态影响，不能当作原始故障的同一根因。

开机后 Rockit 的闲置 venc/ivs/vpss 工作线程会合法地长时间等待 `rkos_down`，
10秒 hung-task 自动报警在相机启动前就大量出现。它们与真正的 CPU0 hardlock 区分记录。

原始日志被 git ignore，保存在 `logs/lifecycle/20261012-debug14/`：
`fault-suite/`、`no-netconsole-suite/`、`irq-affinity-setup.txt`、
`irq3-no-netconsole-70c/`、`irq3-restarts/dual-10/monitor.txt`、
`net-rx-action-disassembly.txt`、`netconsole.txt`。

## 新增 SDK 补丁

Kernel 第18条补丁回移 netpoll 的 RCU 同步修复。参考 Linux stable
[c0e32ec90a9a](https://github.com/gregkh/linux/commit/c0e32ec90a9ab3d64268c57ddf631cd0d3eb0e5d)，
对应 upstream `2da4def0f487f24bbb0cece3bb2bcdcb918a0b72`。
在 netpoll_setup 成功路径释放 RTNL 后等待旧 NAPI poll 离开 RCU 临界区，
再允许 netconsole 发送。SDK 的错误路径上下文不同，手动适配并保留原有引用释放行为。
这用于消除已知日志通道竞态，不声称解决双摄硬锁。

Kernel 第19条补丁新增默认关闭的 `CONFIG_TELEOP_QSPINLOCK_DIAG`，仅
`ar0234-stereo-debug` profile 开启。慢速自旋锁入口保存独立 per-CPU 记录：
锁地址、初始锁值、pid/comm、IRQ状态、12层调用栈；每CPU4个槽保留嵌套进入。
成功拿到锁后清除此记录；hardlock notifier 先打印目标 CPU 的活动记录，前缀 `qspin-diag:`。
不在报告中解引用锁地址，不修改 `spinlock_t`、task 或厂商模块对象布局，
没有启用 lockdep/DEBUG_SPINLOCK。它记录的是等待者，不能直接确定锁持有者。
栈采样会增加争锁路径开销，因此只用于诊断，不能作为性能 baseline。
未启用配置时不保留这些记录；独立关闭配置的 qspinlock.o 编译也通过。

诊断 fragment 将 `CONFIG_DEFAULT_HUNG_TASK_TIMEOUT` 改为0，减少闲置 Rockit 线程的
自动报警噪声，hung-task 功能仍在，可按需手动设置。保留 PREEMPT/HZ1000、stacktrace、
netconsole，以及默认关闭的 lockup/stall panic。没有新增 ramoops 后端。

## 已构建的 #16 镜像

内核为 `#16 SMP PREEMPT Mon Oct 12 20:51:59 CST 2026`。Ubuntu22.04 Distrobox 中
`./build.sh driver && ./build.sh firmware` 成功，内核和外部模块一起重建。
5个关键 KMPP/Rockit 模块 vermagic 均为 `6.1.141 SMP preempt mod_unload aarch64`。
boot.img 为11,390,976字节，未超过11MiB；其余定长镜像也未超对应分区。
SDK仍有两处厂商 KMPP version_history 数组越界编译警告，未修改；本轮探针没有读取这些节点。

当前烧写目录是 `../aura-sdk/output/image`；独立备份是
`artifacts/lifecycle-spin-debug-20261012/`，包含所有镜像、vmlinux、System.map、config和sender。
[SHA256记录](baselines/2026-10-12-lifecycle-spin-debug.json) 已保存。
SDK 63个基线版本校验、19/2/2/9条补丁干净重放及重复应用、8项补丁工具测试、
7项相机launcher测试、netconsole receiver回环测试、Python语法检查均通过；
runtime check 和全量烧写脚本 dry-run 通过。尚未对 #16 做硬件运行验证。

同事复现源码/config的步骤：

```sh
./tools/sdk-patch apply ../aura-sdk
./tools/select-teleop-board ../aura-sdk ar0234-stereo-debug
distrobox enter aura-sdk -- bash -c 'cd /home/ww/codings/aura-sdk && ./build.sh driver && ./build.sh firmware'
```

这不保证镜像逐字节一致；具体限制见 [SDK复现说明](sdk-reproduction.md)。

## 烧写后下一步

请用户手动进入 USB Loader 并执行：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/flash-aura-emmc
```

全量烧写覆盖 userdata。用户恢复 SSH 后再部署程序/脚本，先验证 uname 与启动的
`qspin-diag: contested-lock stack capture enabled` 标记，再在启动相机前设置 watchdog
和网卡 IRQ 亲和性。先不开 netconsole 运行短测试，以 SSH 保存内核日志；需要时再做 netconsole 对照。
若再次硬锁，优先读取 qspin-diag 的锁地址与栈，并用本版 vmlinux 符号分析调用者，
再决定针对驱动修改。不会自动刷写、重启、长测，也不会先通过大规模裁剪掩盖故障。
