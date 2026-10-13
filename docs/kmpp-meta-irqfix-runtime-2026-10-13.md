# KMPP IRQ修复实机回归：短测通过，退出资源正常回收

用户烧写最新镜像后，USB和SSH核对板端为内核6.1.141 #16，
`/oem/usr/ko/kmpp.ko` SHA256为
`b75dda4ef396acac8ca8eacdef2203c2bb540fd783a564cc2577788079c618aa`。
此次镜像含definition、meta、分配记账IRQ锁保护及pool快照补丁。
重新部署sender、diagnose-ar0234、run-ar0234-stereo；sender SHA256仍为
`332d6dd6e1a2aec232e006d51fe8cc51adaca04b6f8d7e956594b7a37d280f56`。
没有修改摄像头驱动、DTS、VBLANK或程序。没有自动重启或烧写。

结论：本轮22个有界用例全部达到预期结果；上轮完整采样流程在mono1触发的
meta锁硬锁没有再现。17次完整网络/硬解测试共3120帧，全部UDP完整、VAAPI硬解，
没有格式错误、重复包、丢帧或解码警告。其余5个用例验证异常和信号退出。
这证明当前短测序列通过，不代表已经完成长时间运行或所有竞争条件的验证。

## 测试顺序与观测条件

两路DECXIN AR0234，1280×720、约99.23fps，30Mbps、GOP15、slice120、wrap0。
每个正常用例每路120帧。启动温度限制70°C，离散before/after快照范围44.16–46.88°C，
不把它当作连续记录的最高温度。主机接收与VAAPI解码在CachyOS原生运行；
目标二进制沿用上一轮Ubuntu22.04 Distrobox交叉编译产物。

没有启用netconsole；USB `adb shell` 读取/dev/kmsg，确认唯一标记后才启动测试。
前半轮watchdog_thresh=3、全CPU软锁回溯开启，USB IRQ73暂放CPU2；
网卡IRQ71/72始终保持启动默认0-3。最后恢复watchdog_thresh=10、回溯关闭、
USB IRQ73=0-3，再完成默认设置回归与真实编码期间的pool读取。

| 阶段 | 用例数 | 结果 |
| --- | ---: | --- |
| minimal双路→camera0→camera1 | 3 | 无阶段指标/线程栈采样，全部通过 |
| 完整生命周期套件 | 10 | 正常启动、异常回滚、进程锁、信号、恢复采集全部通过 |
| 完整采样双路→0→1→双路 | 4 | 全部通过 |
| 初始化期间pool读取 | 1 | 双路通过，20次读取成功；不能据此证明编码IRQ期间读取 |
| 恢复默认诊断设置后双路→0→1 | 3 | 全部通过，原失败顺序没有再现 |
| 第二路出流后pool读取 | 1 | 双路通过，30次读取成功，快照中packet used=4–8，确认编码期间并发读取 |

完整生命周期套件的正常序列为dual-start、mono0、mono1、dual-after-mono；随后：

- 在camera1.vi.enabled、camera1.venc.created、camera0.stream.acquired三个检查点注入失败。
  sender退出1是预期结果，均完成WaitFreeMB和SYS_Exit；已取得的stream正确Release。
- 第二路EnableChn期间TERM，以及第二路ready后INT，均正常回收并退出0。
  后者同时验证另一个sender被进程锁拒绝，不再进入第二次SYS_Init。
- 最后dual-recovery完整接收并硬解两路各120帧。

所有19个开启生命周期trace的正常/异常用例均保存了成功WaitFreeMB及SYS_Exit结尾；
另3个minimal用例不打开trace，使用退出码、完整硬解和测试后的板端响应验证。
USB日志没有hardlock、softlock、BUG或WARNING调用栈。
现有CIF“vblank need >=1000us，cur286us”提示仍会出现；本轮不调整用户已验证的sensor时序。
早前verbose netconsole触发的netpoll警告也没有在这轮验证其修复，继续保持netconsole关闭。

## 资源回收

初始化和每个非minimal用例结束后保存MPP session及KMPP pool信息。
最后MPP session文件为空，sender已退出，帧、包、meta、编码配置等pool的used均为0。
KmppBufGrp和KmppBufGrpCfg各保留1个used，与启动前一致，是模块常驻对象。

pool缓存total会按运行峰值增长，不能将缓存误判为泄漏：
KmppMeta(952字节)最终19个unused、KmppPacket最终18个unused；
从repeat-01-dual开始，到后续混合/默认设置/并发读取测试均保持19/18。
本轮未发现活跃对象泄漏或持续增长；长时间内存稳定性尚未验证。
pool读取没有快照截断或分配失败提示。最后已恢复临时诊断设置并停止日志读取，
板端保持在线且没有camera sender或监控进程遗留。

## 软件耗时基线与下一步

以下取默认IRQ/watchdog设置的default-01-dual，均为同一进程CLOCK_MONOTONIC阶段耗时，单位ms。
不是板端到主机的单向网络延迟，也不是screen-to-screen；分位数是直方图桶上界。

| 指标 | camera0 mean / p95 | camera1 mean / p95 |
| --- | --- | --- |
| sender AU间隔 | 10.072 / 10.224 | 10.072 / 10.224 |
| sender first_pack_to_send | 10.072 / 10.224 | 10.072 / 10.224 |
| sender udp_queue | 0.337 / 0.647 | 0.358 / 0.598 |
| host reassembly | 0.292 / 0.614 | 0.293 / 0.508 |
| host parser_wait | 9.995 / 10.486 | 10.028 / 10.486 |
| host submit_to_frame | 0.097 / 0.143 | 0.124 / 0.205 |

这些短样本不是优化前后的统计比较，不能声称IRQ修复使延迟降低。
但sender发送边界等待和host parser_wait各接近一个帧周期，值得优先审查。
后续应先核实slice/PTS/帧结束标记及parser的timestamp关联，再决定是否能提前提交；
不能把每个slice或UDP分片直接标成完整H.264帧。保持损坏码流、双路隔离、末帧flush测试。
确认应用缓冲边界后，再分组裁剪内核/Buildroot，并用相同软件指标做A/B对照。

本轮不再要求烧写镜像，当前板端可继续使用。下一轮仍保持短测；若重新捕获硬锁，
保留USB调用栈后停止，沿实际锁持有/IRQ上下文定位，不以跳过SYS_Exit绕过问题。

[结构化测试、资源、指标及日志哈希](baselines/2026-10-13-kmpp-meta-irqfix-runtime.json)。
原始日志在ignored的`logs/lifecycle/20261013-meta-runtime/`；补丁和镜像指纹参见
[上一轮修复记录](kmpp-meta-lock-irqfix-2026-10-13.md)。
