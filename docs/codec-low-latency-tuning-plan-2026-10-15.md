# 固化120fps启动参数与下一轮编解码调优

2026-10-15。先保留已经验证的双路相机时序，再通过H.264参数做“以适量带宽换更少计算”的A/B。
本轮已完成默认参数固化、部署和短测；下文编解码参数、协议与测量工具的扩展是下一轮计划，
没有把候选项直接改成生产默认值，也没有运行这些候选组合。

后续同日已实现参数入口并完成主要参数的实机分组测试，详见
[实机结果与使用方式](codec-parameter-results-2026-10-15.md)。本文保留测试前的计划，
渐进slice协议和无线等仍未实现；候选没有取代正常codec默认。

## 已固化并部署

`tools/run-ar0234-stereo`及板端`/userdata/run-ar0234-stereo`现在默认：

| 项目 | 值 |
|---|---|
| 分辨率/相机 | 双AR0234、每路1280×720 |
| sensor VBLANK | 506，约119.95fps；启动时显式写入 |
| VENC声明fps | 120 |
| exposure | 735行，即4.998ms |
| analogue/digital gain | 48 / 512 |
| test_pattern | 0 |
| H.264 | High、CBR 30000kbps/路、GOP15 |
| VI/VENC wrap | 0，保留已验证的双路正确性 |
| slice / 输出 | 120 MB、split mode1、完整AU立即发送 |
| 网络 | 192.168.1.2:5000、原1200B UDP datagram |

启动不再需要传入上述环境变量：

```sh
/userdata/run-ar0234-stereo
```

环境变量覆盖保留，例如恢复旧约99.23fps：

```sh
VERTICAL_BLANKING=762 FPS=100 /userdata/run-ar0234-stereo
```

`--dry-run`会显示FPS、码率、VBLANK与曝光/增益，不写控件。
这份默认配置要求已刷入#19 fastreadout或兼容新驱动；单独的通用`teleop-sender`二进制
仍保留原MIA1321参数，不会自行设置sensor曝光和帧率。通过双摄launcher使用AR0234配置。
固化位于Git中的启动脚本，`tools/deploy-aura`也会部署它；本轮没有改内核上电默认值、
没有新增开机自启、没有重打镜像。旧镜像内的文件不会因为Git提交自动更新，全量刷写后需重新部署。

验证：Ubuntu22.04 Distrobox中现有launcher回归8项通过，覆盖默认配置、两路时序先于曝光、
环境变量覆盖、预检失败与子进程退出。板端清除相机参数覆盖变量后直接运行launcher各120帧，
只为避开日常端口将PORT设为5500。240帧全部VAAPI硬解，malformed/duplicates/dropped0，
队列峰值各1、溢出0，USB活标记后无新增内核/CSI错误；boot ID未变，结束温度47.98°C。
测试结束保留新735/48/512/506控件，sender已退出。
[固化验证摘要](baselines/2026-10-15-ar0234-defaults.json)，
[120fps时序A/B](ar0234-120fps-ab-2026-10-15.md)。

## 当前开销与边界

旧#16、约99fps、High/CBR30Mbps/GOP15下，实际VENC硬件周期均值约2.51ms，
双路AU提交到VAAPI surface真正完成约1.83ms。见
[完成时间测试](codec-completion-and-pyrowave-budget-2026-10-14.md)。
这些是不同阶段均值的估算，不能相加当作同步跨机单向实测；还需重建当前#19/120fps基线。
最新viewer返回AVFrame对象约0.1ms级，仍不代表GPU解码已完成。
GetStream阻塞时间还包含等待下一帧，也不是纯编码耗时。

本地源码确认：sender已通过DMABUF和SYS_Bind连接VI/VENC，完整AU到达后立即发包；
viewer直接提交完整AU、每路一个解码worker、FFmpeg thread_count=1、VAAPI→Vulkan DMA-BUF。
当前不需要为追求参数名称再增加CPU像素下载、编码前复制或解码前一帧lookahead。

PyroWave思路在本平台的对应实验是减少预测/熵编码等工具、独立帧和较宽松的率控，
不是预先保证某个H.264选项能把固定硬件变成0.2ms。
GOP15的P帧引用过去帧不等于等待未来帧；改成GOP1可能改善独立性和恢复，
但无B帧路径并没有15帧重排延迟可消除。应测实际硬件周期和AU大小。

## 先扩展哪些参数

下一轮在sender中提供以下可选CLI，并贯通launcher与测试工具；下列名称为设计，**目前尚未实现**。
保留兼容默认，用独立实验二进制先验证。setter显式请求失败应报错清理，不能静默忽略。

| 入口 | SDK/API与实现要求 |
|---|---|
| 现有`--gop`、`--bitrate-kbps` | launcher增加GOP透传，测试报告保存实际值；现有BITRATE_KBPS继续使用 |
| `--rc-mode cbr\|fixqp`、`--qp-i`、`--qp-p` | `VENC_RC_MODE_H264FIXQP` / `stH264FixQp`；FIXQP按内容变化，不能宣称受bitrate硬上限约束 |
| `--max-reencode` | Get/SetRcParam的`stParamH264.s32MaxReEncodeTimes`；先读回，候选0；若原值0则不重复制造无效对照 |
| `--entropy cabac\|cavlc` | Get/SetH264Entropy的`u32EntropyEncMode`；先仅改熵编码，保留High以隔离变量 |
| `--transform 4x4\|auto` | Get/SetH264Trans的`u32TransMode`，1为只4×4；保留其余读回字段 |
| `--deblock on\|off` | Get/SetH264Dblk的`disable_deblocking_filter_idc`；使用码流信号让两端一致，不在解码端私自跳过滤波 |
| `--profile high\|main\|baseline` | 创建通道前设置；Baseline显式使用CAVLC/4×4，拒绝冲突组合；读回及SPS/PPS验证 |
| slice配置 | 保留现有`--slice-ctus`；增加明确的关闭拆分选项，并将slice大小与输出模式分开记录 |

头文件公开接口不等于当前Rockit二进制/硬件支持全部组合。先在已创建、未StartRecv/Bind的
通道上Get→修改目标字段→Set→Get，保存实际值。再用码流SPS/PPS/slice header核实profile、
entropy、transform、deblock、帧型；ffprobe检查无B帧，不能只信日志的CLI回显。
GOP属性目前只设MaxLtrCount=0，enum仍为INIT；下一轮记录实际模式并验证普通无B预测行为。

## 分轮矩阵

每轮只做少量每路120帧短测，组内A→B→A；先单路确认，再双路验证胜出项。
完整矩阵不在一次会话连续全部跑完。固定#19、VBLANK506、735/48/512、wrap0和电源/IRQ策略。

1. **120fps基线与能力快照。** 保存默认RcParam/entropy/trans/deblock/split、VENC频率、
   码流头、硬件计时和主机surface完成时间。新测量工具必须显式传递sensor参数、VENC120，
   native probe回放间隔改为约8337µs。旧`test-codec-latency`仍硬编码100fps/10000µs且未贯通
   VBLANK，不能直接复用它的旧`--board`命令冒充120fps基线。
2. **优先验证用户提出的GOP1与固定QP。** 先CBR30Mbps/GOP15→只改GOP1，记录质量/码率是否失控；
   再在GOP15下CBR→FIXQP26/26，视码率尝试30或22；最后组合GOP1+FIXQP。
   全I在同30Mbps下可能显著损失画质，需另做CBR40/60Mbps补偿组，不能把压糊的结果称为胜出。
   同轮检查max-reencode，非0时先单独测设0，区分避免重复编码和改FIXQP的收益。
3. **编码工具减法。** 在同一码率/GOP下先单测CAVLC、只4×4、关闭deblock，再组合胜出项；
   Baseline作为组合配置核对兼容性。记录块效应、细字、运动和暗部高增益噪声，不默认更简单就更快。
4. **slice粒度。** 先保留mode1，比较120、240、480 MB及关闭拆分，测纯硬件周期、GPU完成、码流大小；
   不混入协议变更。更小slice增加头部和工具约束，也不保证VAAPI能更早交付整帧。
5. **渐进slice传输。** 独立实现下节协议/边界验证，再与最佳完整AU配置对照，优先尝试消除
   “编码最后一块完成后才发送全部数据”的串行部分。保留旧完整AU模式随时回退。
6. **无线候选与最终组合。** 从胜出配置做更低码率/更长GOP对照，随后才接入实际板载Wi-Fi，
   测可用吞吐、突发排队、重传和丢包恢复；以无线实际测量决定参数，不依据PHY标称速率设置码率。

GOP15/30/60在120fps时，IDR间隔分别约125/250/500ms（持续丢包时恢复可能更久）；
这是有损链路恢复预算，不是正常P帧显示延迟。GOP1每帧独立，也不能修复当前帧自身缺片。
不要同时打开复杂参考结构、B帧、额外lookahead；这些不符合本轮目标。

## slice拆分为什么还要单独做

用户提到的MPP split能力在本地低层MPP中对应`MppEncSplitMode`、`split:mode/arg/out`和
`MPP_ENC_SPLIT_OUT_LOWDELAY`；Rockit MPI暴露的是`VENC_SLICE_SPLIT_S`。
**两套enum数值不是同一语义，不能直接照抄整数。** SDK头文件定义：

- MPI mode1按MB/CTU切片，但所有slice仍在一个packet输出，当前生产正在使用。
- MPI mode3逐slice输出；头文件还声明此模式最多8个slice，而mode1最多501个。
  720p H.264有80×45=3600个MB，120 MB/slice约30片。
  低延迟模式先按480/960 MB（约8/4片）探测，实际限制必须从硬件输出验证。
- 低层MPP的`check_low_delay_output()`在启用LOWDELAY时将reencode、drop、super-frame模式清零；
  这不是单独打开slice数量就必然启用的路径，也不证明当前MPI已经透传所有标记。

历史实测mode3的每个pack都可能`bFrameEnd=true`；现有兼容路径用下一PTS确认上一帧，
会重新增加约一帧等待。见[帧边界修复](frame-boundary-optimization-2026-10-13.md)。
所以不会把`OUTPUT_MODE=slice`直接固化为低延迟默认。

真正渐进发送需要可靠EOI：先核实MPI能否保留低层`mpp_packet_is_partition/is_eoi`语义，
必要时单独做低层MPP实验后端；若拿不到可靠末片标记，本轮不绕过边界检查。
协议设计要支持同帧未知最终长度的slice序号、明确EOF、SPS/PPS/IDR归属、超时/缺片和每流状态，
兼容旧完整AU版本；不能把每个slice都标为CompleteAccessUnit。
先让网络传输与编码重叠、host收齐同一AU后立即提交VAAPI；不要预设FFmpeg/VAAPI支持逐片
边到边出整帧，更不要假定Vulkan可安全显示尚未完成的解码surface。
如果进一步做slice级硬解，应另查后端真实支持并实测同步/参考帧正确性。

本轮仍保留wrap0。它限制VI→VENC的行流水收益；GOP/QP/slice改动不会自动消除整帧采集等待。
此前双路wrap有错图，恢复它属于另一项正确性排查，不与编码参数A/B混合。

## 带宽约束与胜出条件

先保持现行30Mbps/路，候选40、60Mbps/路；80Mbps/路只在有明确收益但较低档不足时再测。
单位均为Mbit/s，不是MB/s。千兆直连预算如下，使用名义120fps、两路共享链路，
按1200B UDP含40B项目头、1160B视频payload及IPv4/UDP/Ethernet开销估算约1.10倍率：

| 每路视频目标 | 双路合计 | 每帧周期合计线占用，千兆 | 相对30/路新增 |
|---|---:|---:|---:|
| 30Mbps | 60Mbps | 约0.55ms | — |
| 40Mbps | 80Mbps | 约0.73ms | 约0.18ms |
| 60Mbps | 120Mbps | 约1.10ms | 约0.55ms |
| 80Mbps | 160Mbps | 约1.47ms | 约0.92ms |

这些是平均序列化预算，不是实测单帧端到端延迟。CBR并非逐帧恒定大小，FIXQP更没有硬码率上限；
IDR/高噪声/运动突发和两路相位决定实际排队。必须保存每AU大小、I/P类型、实际Mbps、
20ms/100ms窗口码率及最大发送突发，而不只记录配置码率。
无线有额外争用、ACK和重传，不能沿用此千兆表推断无线延迟。

晋级依据：画质满足任务、两路内容独立正确、120帧/路完整硬解、无新增内核/CSI或解码警告、
RX和AU队列无溢出，硬件/完成时间改进可重复且不是observer effect。
在上述带宽档位优先保留有实际codec完成收益的最低码率；若只省几十微秒却增加数倍码率则不晋级。
FIXQP遇码率突发先停止该实验，不临时积压大队列或丢弃任意P帧来伪造低延迟。

## 测量与交付要求

板端延用KMPP硬件cycle计数与run→done IRQ计时，另存sender取包→首/末发包阶段；
主机采用原生CachyOS的surface query+`vaSyncSurface2`完成探针，另存AU队列等待与对象返回。
显式同步仅放诊断探针；生产viewer保留异步，不能把逐帧CPU等待当成优化。
首帧/预热单列，I/P帧尽量关联；若kernel session与具体帧未对齐，明确记录不可逐帧匹配。
开始先支持一种120fps双路实录的同步回放，不额外重复生成1080p/4K探针来增加测试负担。

USB日志活标记必须先通过；无风扇每路120帧、启动<70°C、只管理自己进程，
debug/console设置finally恢复；故障即停，不自行重启/烧写。
每轮保留可回退的二进制/配置和紧凑JSON，原始CSV/录像/构建产物放ignored artifacts，
源码/计划/结论入Git并使用本机身份提交。
sender和fake MPI测试在Ubuntu22.04 Distrobox，viewer和真实GPU probe在CachyOS原生。
尚不承诺总共能省几毫秒；软件候选通过后再由光学screen-to-screen验证最终效果。

源码依据（SDK相对本仓库的`../aura-sdk`）：
`media/rockit/rockit/mpi/sdk/include/rk_mpi_venc.h`、`rk_comm_venc.h`、`rk_comm_rc.h`；
`media/mpp/mpp/inc/rk_venc_cmd.h`、`mpp_packet.h`；
`media/mpp/mpp/mpp/codec/mpp_enc_impl.cpp::check_low_delay_output`；
`sysdrv/drv_ko/kmpp/kmpp/mpp_service/mpp_rkvenc2.c::rkvenc_isr`。
