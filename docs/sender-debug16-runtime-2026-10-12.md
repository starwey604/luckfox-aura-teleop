# #16 实机：生命周期通过、netconsole 对照在 SYS_Exit 内失联

用户烧写后确认板端运行 `6.1.141 #16 SMP PREEMPT Mon Oct 12 20:51:59 CST 2026`，
启动日志有 `qspin-diag: contested-lock stack capture enabled`。
部署上一轮同一 sender（SHA256 `36468d1e7192db7274718b181fb20d631ee27fbeed367d9c96f0f1f706cf07aa`）
及相机脚本；每路1280×720、约99.23fps、每用例120帧、wrap=0。
没有修改传感器模式、烧录或自动重启。

## 测试结果

启动任何相机前设置 `watchdog_thresh=3`、all_cpu_backtrace=1，eth0 IRQ71/72
亲和性为CPU3。hung_task_timeout_secs=0。初始温度48.26°C，工具上限仍为70°C。

| 测试条件 | 结果 |
| --- | --- |
| 无 netconsole：整套生命周期测试 | 通过正常双路/单路/再次双路、3处故障注入、TERM/INT及进程锁、双路恢复 |
| 无 netconsole：额外10次双路120帧重启 | 全部成功出流、240帧完整VAAPI解码、清理后SSH正常 |
| netconsole开启，console loglevel=8：dual-start | 通过 |
| 同条件：mono0 | 通过 |
| 同条件：mono1 | 120帧采集/硬件解码完成；在SYS_Exit中失联，SSH退出255 |

这些通过用例仅证明短时路径可工作，不足以宣称内核竞态已修复；慢速锁插桩本身也改变时序。
正常恢复用例CMA回到 CmaFree=278448kB（CmaTotal=278528kB），没有观察到累积CMA泄漏。

失败时最后一组应用日志：

```text
END RK_MPI_VENC_DestroyChn(options_.venc_channel) rc=0
END RK_MPI_VI_DisableChn(options_.vi_pipe, options_.vi_channel) rc=0
END RK_MPI_VI_DisableDev(options_.vi_device) rc=0
END RK_MPI_SYS_WaitFreeMB() rc=0
BEGIN RK_MPI_SYS_Exit()
```

SYS_Exit没有返回。netconsole最后的内核消息是 `vrgn_release 30`，未收到活动锁诊断或
watchdog调用栈。SSH监控看到主线程持续R状态、系统态CPU时间增加，最后温度50.8°C；
运行中的任务栈读取只给出 `0x0`，不能用它确定执行位置。
这次仅能定位到退出阶段，不能认定已复现上一轮同一CPU0硬锁，也不能确认netconsole是唯一原因。

厂商 `release_rockit-ko_rv1126b_arm64_asm/vrgn/vrgn.S` 的 `vrgn_release` 在
`rkos_spin_lock` 与 `rkos_spin_unlock` 之间调用 `rkos_printk`；
`osal/linux/osal_spinlock.c` 使用 `spin_lock_irqsave`，`osal_debug.c`直接调用 `vprintk`。
这让普通日志的同步控制台输出成为需要对照的干扰项。尚未改厂商汇编或锁实现。

## 采集工具调整及下一轮

`capture-aura-kernel-log` 新增 `--console-level 1..8`，默认1：仅 emergency 消息进控制台，
qspin诊断使用pr_emerg，仍可发送；自测标记也改为emergency，确认UDP通道实际收到。
完整内核消息仍保存在ring buffer，可通过独立SSH读取 `/dev/kmsg`，不会只剩紧急日志。
退出时恢复旧控制台level；显式 `--console-level 8` 可以重做本轮普通日志对照。
该改动只涉及本机工具，不需要重编或烧写镜像；不是内核故障修复。

默认/显式verbose两个真实UDP回环测试（fake SSH）在Ubuntu22.04 Distrobox通过，
验证marker接收、选用的level、目标删除和旧level恢复；Python语法及diff检查通过。
紧急级别方案的板端运行还需用户手动重启后验证。

下一步先保持netconsole关闭再运行短测；之后启用默认level1的netconsole作对照。
如果只有普通日志输出时失联，再针对同步printk/网络轮询路径插桩；如果无netconsole也复现，
继续读取qspin锁地址/调用栈并分析驱动。没有用跳过SYS_Exit或直接_exit来掩盖资源释放问题。

原始日志被ignore，位于 `logs/lifecycle/20261012-debug16/`。
[可提交的结果、日志摘要哈希与软件阶段指标](baselines/2026-10-12-debug16-runtime.json) 已保存。
代表性的两路AU周期约10.06/10.07ms，本机AU就绪后等待解码帧约10ms，其中parser_wait占约10ms。
这是后续延迟优化线索，不是硬件解码器耗时，也不是screen-to-screen或板到本机的测量。
