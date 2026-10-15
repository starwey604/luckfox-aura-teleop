# AR0234 读出时间：SSH 实测基线

2026-10-15，旧内核 `6.1.141 #16 SMP PREEMPT`。两路 DT 均为两通道；
`CONFIG_VIDEO_AR0234=y`，驱动内置，HBLANK/pixel_rate/link_frequency 只读。
**当前镜像不能通过在线 V4L2 控件改成四通道或 90MHz 读出。**

## 结果

| 请求曝光 | 实际曝光 | camera0 读出均值 / p95 | camera1 读出均值 / p95 |
|---|---|---|---|
| 1ms，74 行 | 1.0064ms | 9.780 / 9.799ms | 9.783 / 9.794ms |
| 3ms，221 行 | 3.0056ms | 9.789 / 9.790ms | 9.795 / 9.796ms |

每组由一个 MPI sender 进程发送两路，各 120 编码帧；native CachyOS viewer
接收 240 个 AU，两路各 120 帧均为 VAAPI 硬件解码，malformed/duplicates/dropped=0，
没有解码警告、内核 WARN/Oops/lockup 或 CSI 错误。同一次启动完成，控制值恢复到
本次测试前的 exposure514、analogue_gain13、digital_gain128、VBLANK21、test_pattern0。
不要把旧轮次的 74 行曝光当成这次重启后的初值。测试前温度约 39°C，全部结束约 44°C。

这是约每 50ms 读取一次 CIF procfs 的**统计快照**，不是全部帧的采样分布：
在两路 sender 都完成初始化的活跃窗口中，camera0 每组20个、camera1分别21/20个不同帧号的有效快照。
第二路初始化期间第一路已开始采集，所以物理采集帧数多于应用消费的 120 编码帧；只按双方初始化完成后板端uptime筛选快照，排除商家初始化期间的默认曝光。
两组有约十几微秒的均值差异，不能据此认定曝光影响了毫秒级读出。
[精简数据](baselines/2026-10-15-ar0234-readout.json)。

CIF `rkcif_deal_readout_time()` 在帧结束处理中计算
`fe_timestamp - fs_timestamp`，`rkcif_deal_sof()` 等路径记录开始时间。
这里得到的是接收驱动 IRQ 时间差，包含中断处理偏差，不是示波器测量的精确引脚时间，
也不是曝光结束→ISP 输出或 screen-to-screen。它与 720×13.6µs≈9.792ms 一致。
`procfs.c` 使用 `%u.%u` 打印毫秒与微秒余数，未补零；脚本按整数微秒余数解释，
避免将 `9.8 ms` 误读成 9.8ms（实际这种输出表示 9.008ms）。

## 脚本

在原生 CachyOS 执行，使用 `SSHPASS` 或已有 SSH 密钥认证：

```sh
# 只读审核，不启流、不修改控制、不要求 USB 日志。
./tools/test-ar0234-readout --audit-only --output artifacts/readout-audit-new

# 有限曝光对照；必须先连 USB/ADB。目录需不存在。
./tools/test-ar0234-readout --camera dual --frames 120 \
  --exposure-us 1000 3000 --expect-lanes 2 --output artifacts/readout-A-new
```

`--expect-lanes 4` 在旧A镜像上已验证会拒绝测试，不写寄存器、不启流。
它不会将两通道静默伪装成四通道。后续四通道镜像可复用这套脚本，按实际行时间
换算曝光；保持精确物理曝光对照时可指定 `--exposure-us 1006 3006`，然后核对实际行数。

脚本要求先通过 USB `/dev/kmsg` 活标记验证；用现有 MPI 路径而非直接 MMAP 抓图；
每轮不超过两路各 120 编码帧，起始温度限制 70°C，结束恢复原控制并核对 boot ID。
它不会烧录、重启、直接写 I²C、切 PLL/lane，或改变电源/调度策略。
基础软件测试工具新增 `--exposure-lines`，默认仍为735；minimal 与 launcher 路径均贯通。

第一轮脚本因笼统匹配 `Warning:`，将已有的
`vblank need >= 1000us ... cur 286 us` BSP 提示归为失败，停止了后续测试；
这轮也完整收到了两路各120帧，没有内核故障。核对源码和旧 codec 基线后，
将这一条**完整匹配的已知提示**单独保留，其他 Warning/WARN/BUG/CSI 错误仍停测。
修正后先重新运行两组；之后补上双方sender-ready时间门限，再跑上表两组，排除初始化启停阶段。
因此两通道侧本次共消费1200编码帧，其中最终有效对照480帧。

## 后续四通道结果

随后已完成实验镜像并由用户刷写，单目/双目短测通过，读出约减半：
[四通道实现与A/B记录](ar0234-fastreadout-ab-2026-10-15.md)。以下保留A测试完成时的判断。


四通道实验需同时修改 sensor 初始化/时序、设备树两端 lane 配置，编译新内核和 DTB。
优先维持约99.23fps、相同物理曝光，验证有效输出预算能否约减半；
不要将修改报告 pixel_rate 或直接写 `0x31AE` 当作完整配置。
现有数据只证明两通道基线约9.8ms与曝光近似无关，不证明四通道4.9ms已成功。

Ubuntu22.04 Distrobox 中读出脚本单元测试8项、codec工具回归4项通过；
摄像头/VAAPI 实测在原生 CachyOS 完成。原始日志位于被ignore的
`artifacts/ar0234-readout-20261015/`。
