# AR0234 四通道快速读出：实现与实机 A/B

2026-10-15。两颗 DECXIN AR0234 在 Aura 上已通过四通道、720p、约99.23fps的
有限单目/双目测试。**CIF 读出均值从约9.79ms降至约4.88ms，减少约4.90ms。**
这只是接收侧帧开始→帧结束阶段的收益，没有重新测screen-to-screen，不能承诺光学总延迟同幅下降。

## A/B 数据

A是用户原来启动的`6.1.141 #16 SMP PREEMPT`两通道内核；B是本轮编出的
`6.1.141 #18 SMP PREEMPT Thu Oct 15 12:35:13 CST 2026`，用户手动刷写后通过USB/SSH确认。
两个内核的有效CONFIG项完全一致；没有切换到K1、调整调度/电源、网络或编码器参数。

| 物理曝光 | A camera0 mean/p95 | B camera0 mean/p95 | A camera1 mean/p95 | B camera1 mean/p95 |
|---|---|---|---|---|
| 1.0064ms | 9.780/9.799ms | 4.879/4.898ms | 9.783/9.794ms | 4.874/4.898ms |
| 3.0056ms | 9.789/9.790ms | 4.878/4.938ms | 9.795/9.796ms | 4.889/4.934ms |

B的单目camera0/camera1读出均值分别4.888/4.888ms。先完成单目各120编码帧，再完成
两组双目各120编码帧/路，B总计720编码帧。接收AU数量完整、零malformed/duplicate/dropped，
全部为VAAPI硬解帧，未出现解码错误。B的USB活标记后没有新的WARN/BUG/lockup或CSI错误，
A原有的短VBLANK提示在B也没有再出现。B两组帧周期快照均值10.076/10.074ms，仍约99fps。

这些是50ms间隔的procfs快照，按双方sender-ready后的板端uptime过滤，排除初始化阶段；
每个观测帧号最多取一次。B第一组每路20个快照，第二组每路23个；它们不是全部帧的分布。
CIF统计含IRQ时间偏差，十几到几十微秒的抖动不代表sensor实际读出时钟漂移。
源帧开始/结束也不等于曝光结束/ISP完成。

[A数据](baselines/2026-10-15-ar0234-readout.json)、
[B数据与逐路差值](baselines/2026-10-15-ar0234-fastreadout-B.json)、
[镜像/配置/运行文件哈希](baselines/2026-10-15-ar0234-fastreadout-image.json)。
原始数据在ignored `artifacts/ar0234-fastreadout-20261015/`。

## 实现

新增独立profile，保留原来两通道profile的寄存器顺序、延时及默认值，回归测试逐项对照商家INI。
只在选择`decxin,3478v1-fastreadout`时启用以下处理：

- 仍为RAW10、24MHz输入、450MHz link frequency（900Mbps/lane），改为四条顺序数据通道。
  sensor与接收endpoint同时改为`<1 2 3 4>`，CSI0/CSI1继续使用独立DPHY0/3。
- 基于现有参考驱动的四通道90MHz配置，保留商家PLL/MIPI timing与POLY校正；
  两处SERIAL_FORMAT都设置`0x0204`。
- 在初始化前做软件复位并等待200ms，恢复ROM sequencer；跳过45MHz专用的
  `0x3088=0x81BA`、随后`0x3086=0x3D02`这对写入。保留其余vendor序列。
  不能省略复位后直接假设上一次会话没有留下45MHz补丁。
- 初始`0x30BA`采用参考90MHz值`0x7622`；缓存同步到该值，再由正常增益控件按当前增益更新。
- 行长仍612，内部读出clock90MHz，驱动等效pixel_rate360MHz、HTS2448，行时间6.8µs。
  帧长寄存器1477、VBLANK762，含5行开销总1482行，保持原来的10.0776ms帧周期。
  本profile暂不开放约198fps；先固定帧周期验证读出收益，避免平均处理帧数翻倍。
- 默认曝光1028行，对应原商家514行的同一约6.9904ms物理曝光。控件单位仍为行。

SDK kernel commit `e5eb22406`，BoardConfig commit `017ba32`；分别导出为kernel第21条、
BoardConfig第11条patch。可从锁定的旧SDK基线重放，无需手工复制驱动文件。

## 当前使用

用户已刷好B，并恢复现用sender（SHA256仍为
`06583eb5ab52c824f2d8fab1cbea24839fb81aef547060e9d84eb900456a33bd`）与两个辅助脚本。
换镜像产生的SSH host key通过物理USB读取公钥后更新，没有盲目关闭host-key检查。
未自动烧录或重启。

本轮测试全部结束，B boot_id保持`828eed60-1200-411f-9d5e-b9b7b986f313`，
结束温度约44°C，无sender/采样器遗留。控制恢复为B启流前的
exposure1028、gain13/128、VBLANK762、test_pattern0；没有将测试用短曝光偷偷设为永久默认。

手动开始正常双路链路可在板端执行：

```sh
# 1.0064ms曝光；主机仍用原有双路viewer。
EXPOSURE=148 /userdata/run-ar0234-stereo
# 如要约2ms或3ms：EXPOSURE=294（1.9992ms）或442（3.0056ms）。
```

launcher默认仍为735行，在B下是4.998ms；需要短曝光时显式传入环境变量。
不能继续按两通道时的13.6µs/行换算。

复测B，原生CachyOS、连接USB/ADB后执行，输出目录必须为新目录：

```sh
./tools/test-ar0234-readout --camera dual --expect-lanes 4 \
  --exposure-us 1006 3006 --frames 120 --output artifacts/readout-B-new
```

脚本会恢复本次启动前的控件；曝光行数从实际pixel_rate/HTS换算，而不是写死45/90MHz。

## 构建与验证

```sh
./tools/sdk-patch apply ../aura-sdk
./tools/select-teleop-board ../aura-sdk ar0234-fastreadout
distrobox enter aura-sdk -- bash -lc \
  'cd /home/ww/codings/aura-sdk && ./build.sh driver && ./build.sh firmware'
```

- Distrobox：初始化/错误注入/时序测试8项，诊断脚本测试12项；AR0234 builtin对象和外部ko均以
  `W=1 KCFLAGS=-Werror`编译链接通过，checkpatch无输出。
- 编译并验证单目、双目两通道与实验四通道三套DTB；检查两端lane、互指endpoint、独立路由、
  I²C/时钟/供电、eMMC和以太网。另从最终boot.img中提取实际FDT再次通过四通道测试。
- `./build.sh driver`及`firmware`成功，`check-aura-runtime`通过。SDK其他现存模块仍有编译警告，
  包括KMPP info结构访问和Wi-Fi警告；不把全SDK描述成零警告构建。
- FIT内fdt/kernel/resource三个payload的SHA256与镜像描述一致；boot.img为11,391,488字节，
  小于11MiB boot分区。env.img与A完全相同。
- 完整patch stack在隔离锁定基线上重放并对比源码树通过；重复apply不产生额外提交。
  patch管理回归8项、读出脚本8项、codec工具4项也在Distrobox通过；脚本语法检查通过。

本机保存的完整B镜像在`artifacts/ar0234-fastreadout-20261015/image/`；
以后需再次烧录时才手动使用`tools/flash-aura-emmc --images`指定该目录。
全量烧录会覆盖userdata；该目录旁的`runtime/`保存sender和辅助脚本。
当前SDK选择和output/image已是FastReadout/B，用户机器上的开发板也是B。

这次通过的是有限帧数的链路与读出计时测试，没有增加长时间无风扇测试，也没有逐像素检查图像质量。
后续光学总延迟还取决于曝光相位、ISP/VENC衔接、网络与显示呈现；不直接将4.90ms从旧screen-to-screen数字扣除当作实测。
