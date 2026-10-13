# Sender 测试干扰对照（2026-10-12～13）

后续通过USB实时采集抓到了KMPP对象链表锁等待，候选修复已编译：
[KMPP锁定位记录](kmpp-object-lock-irqfix-2026-10-13.md)。以下保留这轮对照时的判断；
不能再把失联仅归为netconsole本身。

用户反馈持续运行 sender + viewer 稳定，失联集中在测试时。持续推流只初始化一次，
反复短测会重复经过 VI/VENC 创建、销毁、SYS_Exit，并额外使用日志和线程栈采集，
所以二者覆盖的路径不同。当前最强线索是 verbose netconsole 对锁路径的干扰，
没有确认具体死锁锁地址，也没有宣称驱动释放竞态已经修复。

## 固定条件与对照结果

板端仍为 #16，PREEMPT/HZ1000；两路AR0234，1280×720约99.23fps、wrap=0，
每次正常采集每路120帧。本机 viewer 在 CachyOS 原生运行，完整检查 UDP 重组和 VAAPI 解码。
除最后默认IRQ复测外，watchdog_thresh=3、eth0 IRQ71/72亲和性=CPU3，与上轮对照保持一致。

| 条件 | 结果 |
| --- | --- |
| 无netconsole、无阶段统计、无线程栈轮询；双路/0/1交替3轮（10-12） | 9次成功 |
| 仅SYS_Init→WaitFreeMB→SYS_Exit，无摄像头（10-12） | 6次成功清理，故障注入按预期退出1 |
| 修正后的工具`--minimal`，无netconsole（10-13） | 双路120帧成功 |
| netconsole level1，仅紧急输出，`--minimal`；双路/0/1交替3轮 | 9次成功，临时目标正常删除，console level恢复7 |
| netconsole level1，恢复完整原采样/阶段统计/故障注入/信号测试 | 整套生命周期测试通过，双路恢复成功 |
| 恢复默认IRQ0-3、watchdog_thresh=10等，新sender正常双路路径 | 首次连接失去响应且用户断网/手动重启，归为证据不足；重启复测成功 |

完整套件覆盖 dual-start、mono0、mono1、dual-after-mono、三处故障回滚、TERM-during-init、
INT-after-ready与进程锁拒绝、dual-recovery。故障注入预期退出1，其余正常退出0。
与上一轮 [verbose netconsole level8的SYS_Exit失联](sender-debug16-runtime-2026-10-12.md) 相比，
保留采样而降低控制台输出级别后成功，是观察通道干扰的重要证据，仍不是严格的根因证明。

成功用例两路各120帧硬解，malformed/duplicates/dropped为0。测试后的CMA为
CmaTotal=278528kB、CmaAllocated=80kB、CmaFree=278448kB；未看到CMA累积泄漏。
10-13 quiet/full quiet测试结束时SSH正常、约42.1°C、无活动sender、无临时netconsole目标。
没有新的hard/soft lockup或活动qspin诊断记录。没有做长时负载或screen-to-screen测量。

## 程序与测试工具的改动

sender 增加 `AURA_FAIL_AFTER=system.initialized`。检查点放在 MppSession 构造完成后，
抛出诊断异常时正常执行 SYS_WaitFreeMB 和 SYS_Exit；不打开VI/VENC，也没有跳过清理。
mock MPI测试确认只发生这三个SYS调用且各一次。新版仍可正常双路运行。

正常对照保留板端 `/userdata/teleop-sender` 的上一版二进制，SHA256为
`36468d1e7192db7274718b181fb20d631ee27fbeed367d9c96f0f1f706cf07aa`。
新诊断候选部署为 `/userdata/teleop-sender-system-probe`，SHA256为
`332d6dd6e1a2aec232e006d51fe8cc51adaca04b6f8d7e956594b7a37d280f56`。
当前 `build/aura/bin/aura-sender` 也已重建，具体哈希见结果JSON；未覆盖板端默认程序。
所有sender构建和mock测试在Ubuntu22.04 Distrobox完成。

`test-aura-software-baseline --minimal`：

- 不启动 `/dev/kmsg`/线程栈轮询、stereo.log镜像进程，不读取MPP厂商proc统计；关闭应用阶段统计。
- 直接启动单进程sender，仍保留启动温度检查、占用检查、有限帧数、超时、完整UDP/VAAPI验证。
- 启停前后只读取uptime/meminfo/温度/ps，用于轻量状态确认。不是零干扰测试。
- 匹配I²C4/3上的AR0234，明确检查单/双路节点数量。

首版minimal双路脚本的case模式没有引用传感器名称中的空格，shell在启动sender前报语法错误。
它不属于板端故障。已修正并抽出controls_script；Distrobox执行生成的shell，验证单/双路选择、
排除额外传感器、含空格sysfs路径、VBLANK先于曝光设置、缺少第二路时报错，3项测试通过。
2项netconsole receiver测试、3项CMake/生命周期测试、Python语法和diff检查也通过。
交叉编译和runtime check成功；这轮无需重编SDK或烧写镜像。

## 当前判断与后续定位边界

厂商 vrgn_release 在关中断并持锁期间同步 printk，已在本地汇编/OSAL代码确认。
netconsole会进入网卡轮询路径，所以控制台输出不是被动记录。上游邮件
[netpoll在持rq锁时调用NAPI的死锁讨论](https://lore-kernel.gnuweeb.org/lkml/877bnxfwa8.fsf%40jogness.linutronix.de/T/)
也分析了类似机制，并指出简单禁用BH的方案不足以覆盖所有驱动行为。
这是候选机制参考，未证明本板死锁就是rq锁，也未回移该讨论中的实验性修复。
第18条已有的netpoll启用时RCU修复解决的是另一项竞态。

此前 #14 关闭netconsole后仍有失联和CPU0 qspin等待证据，不能用本轮成功抹掉。
本轮优先修正观察手段；后续默认不使用verbose netconsole。若在minimal、无netconsole条件
仍复现，再用独立SYS探针区分系统退出与相机退出，针对实际锁栈或设备close插桩。
板端已有gdbserver，但本轮没有挂接或暂停正在推流的线程。
继续避免在未取得调用栈时修改厂商汇编、关闭SYS_Exit或大量裁剪内核来掩盖问题。

可复用的短测：

```sh
# 在CachyOS上执行；不带阶段统计或线程栈轮询。
./tools/test-aura-software-baseline --minimal --camera dual --frames 120 \
  --output logs/lifecycle/new-minimal-dual

# 默认console level1；保持自测标记和紧急qspin记录可接收。
./tools/capture-aura-kernel-log --output logs/lifecycle/new-emergency-kernel.txt
```

只检查SYS路径时，在无sender运行的板端执行新版候选：

```sh
AURA_LIFECYCLE_TRACE=1 AURA_FAIL_AFTER=system.initialized \
  /userdata/teleop-sender-system-probe --frames 1
# 预期返回1，同时日志必须有SYS_Init/WaitFreeMB/Exit成功；这是诊断注入结果。
```

[结果和日志摘要哈希](baselines/2026-10-13-observer-controls.json) 已保存。
原始数据在ignored的 `logs/lifecycle/20261012-debug16-minimal/` 与
`logs/lifecycle/20261013-observers/`；测试间断网并手动重启的一次结果按“不确定”保留。
已将实验的网卡IRQ亲和性、watchdog阈值/全CPU回溯恢复#16启动默认值。
没有自动重启、烧录或卸载模块。

## USB备用通道

板端已有adbd与ffs.adb功能，用户安装本机android-tools后，ADB已验证能以root读取
本版uname/uptime/温度和内核日志。当前枚举的通用model字符串是Nexus_4，并不是设备身份依据；
通过USB Rockchip VID及板端内核信息确认目标。下一轮可以在网口对照时用USB独立保存日志，
避免把SSH失联直接等同于整板锁死。本轮仅验证通道，没有通过ADB重启或刷写。
