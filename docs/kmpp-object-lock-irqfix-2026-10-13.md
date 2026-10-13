# KMPP对象锁IRQ重入风险：实机证据与候选镜像

现在已定位到实际等待锁：KMPP对象definition的list_obj链表锁。
CPU0在slice回调申请packet/meta对象时等待该锁，源码却在get/put两侧使用普通spin_lock，
存在同CPU中断打断持锁线程后再次获取该锁的风险。已保存针对两处短临界区的IRQ-save
候选补丁并构建镜像，等待用户烧写验证；锁持有者没有被直接记录，不能宣称唯一根因已证实或已修复。

## 实机对照与日志

内核仍为#16诊断版，程序仍为原板端sender，双AR0234 720p约99.23fps，每次每路120帧。
先前紧急日志/最小采样对照见 [观察通道记录](sender-observer-controls-2026-10-13.md)。

本轮提前把eth0 IRQ71/72分到CPU3、USB dwc3 IRQ73分到CPU2，watchdog_thresh=3，
以便在CPU0停转时尽量保留日志通道。没有修改CPU/NPU频率、曝光、VBLANK或DTS。
以下失败后立即停止，没有自动重启或刷写。

- verbose netconsole level8、最小采样：双路/0/1三个短测全部通过。
- verbose netconsole level8、完整原采样：第一项dual-start就失联，sender/SSH退出255。
  因而verbose日志单独不必然触发，不能只把问题归为观察通道。
- 独立USB日志通过 `adb shell` 实时读取 `/dev/kmsg`，先收到唯一自测标记才启动相机。
  USB与netconsole保存了相同的14条qspin诊断消息。失联后的新增ADB诊断也超时，
  说明USB通道保留了早期dump，但不能保证整板锁死后仍可交互。
- 旧adbd不支持本机 `adb exec-out` 请求，该次读取仅返回error: closed。
  已切换为兼容的 `adb shell`；那份空采集不是硬件故障证据。

核心锁记录：

```text
qspin-diag: hardlocked CPU0 slowpath-depth=1
qspin-diag: cpu=0 slot=0 lock=ffffff8002fe3510 initial=0x1 pid=1467 comm=teleop-sender irq-off=1
_raw_spin_lock
osal_spin_lock [kmpp]
get_obj_from_def [kmpp]
kmpp_obj_get [kmpp]
kmpp_meta_get [kmpp]
kmpp_packet_impl_init [kmpp]
kmpp_obj_get [kmpp]
kmpp_packet_get [kmpp]
mpp_enc_alloc_packet_with_ring_buf [kmpp]
mpp_enc_impl_slice_info [kmpp]
mpp_enc_slice_info [kmpp]
mpp_vcodec_enc_int_handle [kmpp]
```

对应Rockchip硬件PC采样再次显示CPU0停在queued_spin_lock_slowpath；CPU1空闲，
CPU2在synchronize_hardirq中等待。捕获的是等待者，锁地址仅适用于该次启动。
12层栈没有覆盖被中断的原持锁者，因此IRQ自重入是源码支持的候选机制，仍待回归验证。

在卡死之前还有一个独立警告：相机CIF硬中断打印vblank警告，进入netconsole/netpoll，
stmmac_tx_clean调用_raw_spin_unlock_bh，触发__local_bh_enable_ip的
`WARN_ON_ONCE(in_hardirq())`。这证实verbose日志改变了中断内的执行路径。
该警告不能替代KMPP锁证据；本次没有对netpoll/stmmac作猜测性修改，仍默认使用level1。

## 修改范围

KMPP厂商基线：`72ff932e755fee79672ec19d2714369ea8ffed84`。
新增teleop分支 `teleop/kmpp-irq-locks`，提交
`60b6a55c1c19df09c896976ed0e88e7e53f67ff9`，已导出到
`sdk/patches/sysdrv/drv_ko/kmpp/kmpp/0001-...patch` 并加入 `sdk/series.tsv`。

只修改 `sys/kmpp_obj.c`：

- get_obj_from_def 中list_objdef加入definition链表的临界区；
- kmpp_obj_put 中从同一链表删除对象的临界区。

两侧改用已有 `osal_spin_lock_irqsave` / `osal_spin_unlock_irqrestore`，每个调用使用局部
`rk_ul flags=0`，恢复进入前的IRQ状态。原接口会读入flags，因此明确初始化局部值。
仅覆盖短的链表操作，不把对象初始化/反初始化回调或内存分配放进新增IRQ屏蔽区间。
没有改OSAL结构尺寸、共享对象布局、协议、sender退出顺序、wrap/slice或编码参数。
这项修复同时适用于正常与diagnostic profile。

## 构建与交付

Ubuntu22.04 Distrobox中 `./build.sh driver && ./build.sh firmware` 成功，内核和全部外部模块
按现用profile一起构建/打包。KMPP源码对象确实重新编译，打包模块的反汇编显示
get_obj_from_def调用IRQ-save/restore接口。
SDK会strip debug/note/comment，故源码目录未strip模块与打包模块整文件哈希不同；
两者ELF .text完全相同，确认OEM包含本次新代码。

全部63个SDK基线、5个目标的补丁干净重放/重复应用通过，8项补丁工具测试通过。
关键模块vermagic均为 `6.1.141 SMP preempt mod_unload aarch64`。
sender runtime check、全量烧写dry-run通过，全部定长镜像未超分区。
boot.img为11,390,976字节，未超11MiB；KMPP原有两处version_history数组编译警告未改。
没有执行硬件回归，当前板子因锁死无法响应，需用户烧写后继续。

内核源码未变，因此uname仍为 `#16 SMP PREEMPT Mon Oct 12 20:51:59 CST 2026`。
判断是否烧到候选必须核对新KMPP模块，而不是看内核版本号增加：

```text
/oem/usr/ko/kmpp.ko SHA256:
8f027a309020f6aec514eaf4556f906a94686e8ea3436b8bb86e096bcc23babe
```

当前镜像目录：`../aura-sdk/output/image`。
独立副本：`artifacts/kmpp-irqfix-20261013/`，保留全部镜像、关键ko、vmlinux、System.map、config、sender。
[镜像/模块SHA256](baselines/2026-10-13-kmpp-irqfix-image.json) 和
[两条通道的锁证据、原始日志摘要哈希](baselines/2026-10-13-kmpp-hardlock-evidence.json) 已保存。
原始ignored日志为 `logs/lifecycle/20261013-adb-observer/`。

请用户手动进USB Loader，再全量烧写：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/flash-aura-emmc
```

该操作覆盖userdata，烧后需要重新部署sender/脚本。下一轮先核对模块哈希，再用无netconsole
的正常短测和反复退出回归；随后用USB记录紧急日志对照。只有明确的新证据需要时才恢复verbose
输出，并保留它的netpoll警告作为另一项问题。每次仍为120帧、温度门槛70°C，不自动长测。
