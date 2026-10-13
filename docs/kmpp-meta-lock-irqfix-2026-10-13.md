# 首轮IRQ修复复测：meta锁仍会硬锁，补齐同类链表保护

首轮对象definition锁候选已由用户烧写，板端模块哈希确认为
`8f027a309020f6aec514eaf4556f906a94686e8ea3436b8bb86e096bcc23babe`，
模块已加载。内核仍#16；新版sender哈希为
`332d6dd6e1a2aec232e006d51fe8cc51adaca04b6f8d7e956594b7a37d280f56`。
重部署了sender、diagnose-ar0234和run-ar0234-stereo。
新SSH公钥通过USB读取核对后更新known_hosts，没有关闭主机密钥检查。

## 实机结果：还没有解决全部硬锁

网卡IRQ保持启动默认0-3，USB dwc3 IRQ临时分到CPU2，watchdog_thresh=3、全CPU回溯开启。
全程没有netconsole目标；只用USB `adb shell` 读/dev/kmsg，收到唯一标记才开始采集。
两路AR0234 1280×720约99.23fps、wrap=0，所有正常用例每路120帧，启动门槛70°C。
未改DTS、VBLANK、曝光模式或编码参数。

1. 无阶段统计/线程栈轮询的最小测试：dual、camera0、camera1、再6次dual，9次全部通过。
   每次完整UDP接收和VAAPI硬解、正常退出，SSH仍在线；共1920帧。
2. 恢复完整采样的生命周期套件：dual-start和mono0通过；mono1开启出流后失联。
   套件立即停止，没有执行后续故障注入或信号测试。
3. USB在失联前保存了CPU0硬锁和精确等待栈，记录的最近温度约46.8°C。
   这次没有netconsole，证明先前不能仅将故障归为网口日志通道。

本次锁地址和调用者：

```text
qspin-diag: hardlocked CPU0 slowpath-depth=1
qspin-diag: cpu=0 slot=0 lock=ffffff80051b9380 initial=0x1 pid=3959 comm=teleop-sender irq-off=1
_raw_spin_lock
osal_spin_lock [kmpp]
kmpp_meta_impl_init [kmpp]
kmpp_obj_get [kmpp]
kmpp_meta_get [kmpp]
kmpp_packet_impl_init [kmpp]
kmpp_obj_get [kmpp]
kmpp_packet_get [kmpp]
mpp_enc_alloc_packet_with_ring_buf [kmpp]
mpp_enc_impl_slice_info [kmpp]
mpp_enc_slice_info [kmpp]
mpp_vcodec_enc_int_handle [kmpp]
Watchdog detected hard LOCKUP on cpu 0
```

等待点从上一轮get_obj_from_def转到kmpp_meta_impl_init，表明packet/meta申请链有多个
独立的IRQ共享锁。依然没有直接记录锁持有者，不能据此声称唯一死锁机制已经全部确定。
当前板子已失联；没有自动重启、烧写或卸载模块，也无法在失联状态恢复临时诊断设置。
用户重新启动后这些IRQ/watchdog设置自然回到镜像默认值。

## 沿packet/meta申请与释放链审查

| 共享资源 | 原实现及本轮处理 |
| --- | --- |
| 对象definition链表 | 首条补丁已将get/put的两处临界区改为IRQ-save，继续保留 |
| meta全局链表 | init/deinit及模块清理共3处普通spin_lock配对，全部补齐IRQ-save |
| 分配记账链表 | kmem_debug默认启用；pool缺少缓存时原子分配也会进入kmem_add，free进入kmem_del；两处普通锁改为IRQ-save |
| pool get/put/create/drop | 已使用IRQ-save；未改生产路径锁或对象布局 |
| pool统计读取 | 原普通锁和IRQ分配器共享，且持锁执行seq格式化；改为有界快照+解锁后格式化 |
| packet/ring | packet初始化委托meta申请；ring代码没有同类型普通自旋锁，不作整体改写 |

不是把整个SDK的普通锁统一替换。buffer/shm/symbol映射等其它管理路径未作大范围转换；
若后续捕获另一把锁，继续依照实际调用上下文和完整持锁路径定位。

第二条KMPP补丁 `d5d13f82`：

- meta三处使用局部rk_ul flags=0和已有IRQ-save/restore接口；
- 内存记账两处使用spin_lock_irqsave/restore；
- mismatch错误信息移到记账锁外，避免在该锁内调用printk。

第三条KMPP补丁 `99355854`：

- pool统计先在IRQ-safe锁下计算快照容量，在锁外分配内存；
- 再在同一IRQ-safe锁下复制pool/used/unused节点数据，最多4096行；
- 解锁后格式化全部表格，释放快照。名称保存到64字节私有缓冲区；
- 两次锁之间列表增长或超过4096行时，有明确truncated提示；OOM也有提示；
- 仅改变诊断输出实现，不更改共享结构、OSAL接口、wire协议或slice/wrap配置。

快照仍有复制开销，不应把频繁读取完整pool统计当成零干扰性能采样。

## 验证与新候选镜像

Ubuntu22.04 Distrobox编译driver（内核+外部模块）和firmware通过。
初次编译发现缺少minmax头及字符串条件表达式类型警告，已修正并重新完成构建；
本轮修改文件没有新增编译警告。SDK原有version_history数组警告仍未修复。

新增 `tests/aura/test_kmpp_pool_snapshot.py`，从实际SDK源码取出生产快照函数，
在带锁/IRQ状态检测的OSAL shim中用ASan/UBSan编译执行。验证null/空池、正常used/unused，
分配时列表并发增长、5000节点触发行数上限、分配失败，以及分配/格式化不发生在本函数
IRQ临界区内。它没有模拟真实硬件IRQ、不能替代板端回归。
该测试通过，补丁干净重放/重复应用、63项版本检查、8项补丁工具测试通过。

KMPP现在3条补丁均在 `sdk/patches/sysdrv/drv_ko/kmpp/kmpp/`，同事按现有
sdk-patch apply流程即可获得；其余补丁目标未改。sender无需改动或重新链接库。
反汇编确认meta初始化调用IRQ-save/restore；剥离debug的OEM模块.text与编译产物一致。
5个关键模块vermagic匹配 `6.1.141 SMP preempt mod_unload aarch64`。
所有定长镜像未超分区，boot.img为11,390,976字节；runtime check和flash dry-run通过。
尚未对这版新增补丁做实机运行验证。

新模块哈希（uname仍#16）：

```text
/oem/usr/ko/kmpp.ko SHA256:
b75dda4ef396acac8ca8eacdef2203c2bb540fd783a564cc2577788079c618aa
```

镜像仍在 `../aura-sdk/output/image`，独立副本为
`artifacts/kmpp-meta-irqfix-20261013/`，含镜像、关键模块、vmlinux、System.map、config、sender。
[新镜像与模块哈希](baselines/2026-10-13-kmpp-meta-irqfix-image.json) 和
[实机结果与meta锁证据](baselines/2026-10-13-kmpp-meta-hardlock-evidence.json) 已保存。
原始ignored日志为 `logs/lifecycle/20261013-irqfix-runtime/`、
离线构建/测试日志为 `logs/lifecycle/20261013-meta-irqfix/`。

## 下一步需要用户烧写

手动进入USB Loader，再执行：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/flash-aura-emmc
```

全量烧写覆盖userdata。恢复后先验证模块哈希，再部署程序；先用无netconsole、有限120帧
的原失败序列进行针对性回归，随后核对pool统计是否可读/是否有truncated提示和资源泄漏。
USB作为独立日志通道保留；不做自动长测或通过跳过SYS_Exit掩盖问题。
