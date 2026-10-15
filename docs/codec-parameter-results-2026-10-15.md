# 120fps双AR0234：H.264参数实机结果

后续用户已接受画质，GOP15/CAVLC/4×4/关闭拆分已设为正常launcher默认并部署，
见[固化记录与运行方式](codec-defaults-2026-10-15.md)。本文保留候选测试阶段的配置和哈希记录。

2026-10-15。**当前硬件上确实能减少codec阶段时间，但收益约0.4–0.5ms，
没有复现PyroWave那样约0.2ms编码。增加码率或切到FIXQP也不自动加速。**

完成参数入口、实际读回、码流验证和分组短测；未改内核、DTS、电源、IRQ、相机时序，
未刷写或重启。正常`/userdata/teleop-sender`保持原二进制与原codec默认。
参数版单独部署为`/userdata/teleop-sender-codec`。
[结构化结果](baselines/2026-10-15-codec-parameters.json)，
[本轮计划](codec-low-latency-tuning-plan-2026-10-15.md)。

## 测量边界

- #19 PREEMPT内核、1280×720、两颗AR0234四通道，VBLANK506、约119.95fps，
  exposure735=4.998ms、gain48/512、wrap0。
- Aura编码时间读取KMPP帧完成中断中的硬件cycle计数，由驱动按clock换算；
  run→IRQ另存。没有把GetStream阻塞等待当成编码时间。
- 同批真实相机H.264录在板端`/tmp`后取回；主机Radeon 890M、CachyOS原生，
  使用生产FFmpeg VAAPI decoder，query后`vaSyncSurface2`等待surface真正完成。
  双路以8337µs周期、相近相位回放，每份录像3轮；每路每轮排除首10帧，首帧另存。
  这包含提交/GPU工作/同步唤醒，**不是纯GPU timestamp，也不是实时网线下的相机相位**。
- 下表“合计”只是编码和解码分别测得的均值相加，未包含sensor、ISP排队、网络、
  Vulkan显示与扫描；不是同一帧的跨机单向或screen-to-screen实测。
- 每次相机最多120帧/路，组间有本机回放分析；先单路验证新工具/关键setter，再双路。
  每次独立USB日志活标记，退出后恢复debug/console，温度门槛70°C。
  各配置均相同录制/插桩方式，另有一次关闭IRQ计时且不录像的双路控制。

## 双路结果

编码与解码栏均合并两路样本，单位ms。基线重复4次；两个主要候选各重复2次，
中间做回到基线的对照。其它行多为一次双路相机短测加3轮本机回放，不能当作长期尾部保证。
有效码率按录像字节数/相机帧周期计算，双路合计、Mbit/s，不含项目/UDP/Ethernet头。

| 配置，未注明项均沿用基线 | 编码硬件均值 | 解码完成均值 | 阶段均值之和 | 有效总码率 |
|---|---:|---:|---:|---:|
| High / CBR30Mbps每路 / GOP15 / slice120 | 2.515 | 1.842 | 4.357 | 58.2 |
| GOP1 | 2.179 | 1.853 | 4.032 | 61.6 |
| max-reencode=0 | 2.516 | 1.857 | 4.373 | 59.1 |
| FIXQP26/26 | 2.550 | 2.232 | 4.782 | 110.7 |
| FIXQP30/30 | 2.449 | 1.738 | 4.187 | 30.6 |
| GOP1 + FIXQP26/26 | 2.191 | 2.376 | 4.567 | 155.4 |
| GOP1 + FIXQP30/30 | 2.186 | 1.946 | 4.131 | 70.3 |
| CAVLC | 2.496 | 1.706 | 4.202 | 58.5 |
| 只4×4变换 | 2.318 | 1.776 | 4.094 | 59.5 |
| 关闭去块滤波 | 2.534 | 1.785 | 4.320 | 57.9 |
| slice240，mode1整帧输出 | 2.491 | 1.809 | 4.300 | 57.4 |
| slice480，mode1整帧输出 | 2.483 | 1.738 | 4.221 | 57.5 |
| 关闭slice拆分 | 2.476 | 1.696 | 4.172 | 57.6 |
| slice480，mode3逐slice输出兼容路径 | 2.483 | 1.712 | 4.195† | 57.4 |
| CBR40Mbps每路 | 2.527 | 1.960 | 4.488 | 73.7 |
| CBR60Mbps每路 | 2.545 | 2.139 | 4.684 | 103.9 |
| GOP1 + CBR60Mbps每路 | 2.184 | 2.187 | 4.371 | 119.5 |
| CAVLC + 4×4 | 2.311 | 1.760 | 4.072 | 60.1 |
| GOP1 + CAVLC | 2.184 | 1.858 | 4.041 | 61.6 |
| GOP1 + 关闭slice拆分 | 2.177 | 1.785 | 3.962 | 61.3 |
| **CAVLC + 4×4 + 关闭slice拆分** | **2.315** | **1.607** | **3.922** | **59.2** |
| **GOP1 + CAVLC + 关闭slice拆分** | **2.189** | **1.723** | **3.911** | **61.6** |

† mode3还额外存在sender的帧边界确认等待，平均约6.17/6.21ms，**未包含在该行codec合计中**。
对应整帧路径这段约0.1ms。GetStream每次等待从约7.8ms降到约0.97ms，只是每帧返回8片，
绝不等于编码器快了8倍。原PTS聚合需要下一帧第一片来确认上一帧，不能用于低延迟默认。

关闭拆分时，码流实际为1 slice/AU；240和480分别15和8片；原120为30片。
SPS/PPS及slice header验证了CAVLC、4×4、deblock、profile等设置；GOP1录像每路120个key AU，
GOP15每路8个。全部成功录像均无B帧，完成探针逐帧检查AU/frame关联。
头文件中“支持”的参数没有被当作已经生效的证据。

## 两个候选的重复性与取舍

| 配置 | 编码均值 / p95 | 解码完成均值 / p95 | 各轮阶段均值之和 |
|---|---|---|---|
| 基线，4轮 | 2.515 / 2.558 | 1.842 / 2.507 | 4.376、4.339、4.366、4.347 |
| CAVLC+4×4+不拆分，2轮 | 2.315 / 2.331 | 1.607 / 2.275 | 3.872、3.971 |
| GOP1+CAVLC+不拆分，2轮 | 2.189 / 2.192 | 1.723 / 2.275 | 3.888、3.935 |

分位数是合并真实样本后的nearest-rank，没有把各轮p95平均，也不能把编码p95加解码p95
当作总链路p95。主机同步回放仍有调度尖峰，均值收益比尾部保证更有证据。

优先把**保留GOP15的CAVLC+4×4+不拆分**列为画质复核候选；codec均值约改善0.435ms，
本批画面的有效码率约59Mbps。GOP1组合约改善0.445ms，差约0.01ms不足以判定谁更快，
但GOP1每帧独立、丢帧后恢复更方便；相同码率缺少帧间压缩，会影响画质。

FIXQP30虽较快，但码率只有约31Mbps合计，与基线约58Mbps并非相同质量/数据量对照，
不能把全部差值归因于取消率控。FIXQP26使码率增加、解码更慢；固定QP不是加速开关。
CBR40/60也未得到codec收益。建议保持30Mbps/路，除非画质复核表明确有需要。
这些短片的CBR实际均值不是长期严格上限，FIXQP更没有硬码率上限。

当前场景主要是静态箱体、线材和暗部高增益噪声，没有运动标定图或同帧RAW参考。
已抽取每路第60帧作原配置/候选的本机对照PNG，未做无参考PSNR/SSIM，也未声称画质等价。
已查看候选两路图像，未见条带、四分之一黑屏等明显破坏；不替代用户的动态画质验收。
原始PNG与H.264位于ignored `artifacts/codec-parameters-20261015/`。

## 运行与回退

`tools/run-ar0234-stereo`现在可通过环境变量透传GOP、SLICE_CTUS、RC_MODE、QP_I/P、
PROFILE、ENTROPY、TRANSFORM、DEBLOCK、MAX_REENCODE、CODEC_REPORT。
默认仍是原codec配置，双路120fps和735/48/512继续固化；不传新变量就不会启用候选。
新参数需要本轮参数版sender，旧正常二进制不识别新增CLI。

已经上传launcher，并以新环境变量路径做过双路240帧实机集成测试和实际读回。
板端试用保留GOP15的候选：

```sh
SENDER=/userdata/teleop-sender-codec ENTROPY=cavlc TRANSFORM=4x4 SLICE_CTUS=0 \
  /userdata/run-ar0234-stereo
```

试用每帧独立的候选：

```sh
SENDER=/userdata/teleop-sender-codec GOP=1 ENTROPY=cavlc SLICE_CTUS=0 \
  /userdata/run-ar0234-stereo
```

正常viewer启动方式不变。Ctrl+C停流后，以无覆盖变量的新shell运行
`/userdata/run-ar0234-stereo`即可回原codec配置；相机时序仍为120fps。
测试程序未替换正常sender，避免在尚未做动态画质对比前改变日常结果。
本轮不存在开机自启改动。

新同事从源码构建参数版：在Ubuntu22.04 Distrobox运行`./tools/build-aura build/aura-codec`，
将`build/aura-codec/bin/aura-sender`部署到板端`/userdata/teleop-sender-codec`；
同时更新本仓库launcher。主机探针必须在CachyOS原生编译。
`tools/test-aura-codec-parameters`每次只运行一个配置，支持`--no-timing --no-record`控制，
启动前要求明确提供本机编译的`--probe`。参数定义集中在工具的CASES，输出目录必须新建。

本机已有本轮探针时，重测一组的例子：

```sh
SSHPASS=luckfox ./tools/test-aura-codec-parameters --case cavlc-4x4-slice-off \
  --probe artifacts/codec-parameters-20261015/vaapi-completion-probe \
  --output artifacts/codec-parameters-next-check
```

`--codec-report`保存的是StartRecv前的MPI读回，后端仍可能再归一化；码流头与实际IRQ计时
作为进一步证据。FIXQP下旧启动banner的30000kbps不是码率限制，实际数据以RC模式读回
和录像字节数为准。

## 稳定性、异常与未覆盖项

总共44次尝试（含launcher集成），42次通过；合计8640个真实相机帧在主机硬解，
其中包含被拒绝的Baseline profile组120帧。另有24120个本机录像回放硬解帧。
每次均为每路120帧，不是一次长跑；温度结束快照46.29–48.62°C。
成功组均zero malformed/duplicates/dropped、RX溢出0、无新USB内核/CSI问题。

两次未通过如实保留：

- 一次FIXQP26预检遇到/proc进程退出的文件竞争，stderr被误认为活跃应用；没有启流。
  已修正stderr重定向，重新单路测试通过。没有将它当作板子卡死或编码失败。
- Baseline profile66组120帧收发/硬解成功，但触发两条
  `Warning: invalid cabac_init_idc 0 for h264 profile 66, set to -1.`。
  工具立刻停止该组后续矩阵。源码`vcodec/mpp/codec/mpp_enc_impl.c:830`显示为配置自动修正，
  不是内核WARN栈/CSI错误；没有放宽通用告警检查或宣称此组通过。未继续晋级该profile，
  改用High下分别设置CAVLC/4×4的已验证路径。

最终boot ID仍`9183cb9c-73b7-47ff-a8c6-f22ada820f7a`，debug恢复0，console恢复7/4/1/7，
无测试sender或MPP session；池的used仅常驻KmppBufGrp/KmppBufGrpCfg各1，其余0。
正常sender SHA256仍`06583eb5ab52c824f2d8fab1cbea24839fb81aef547060e9d84eb900456a33bd`；
参数版SHA256为`01e55734e2a755c649e61ba13a873cbf31bb93f37b5d72c9036cd48e8a3eb946`。

Distrobox交叉编译、runtime依赖检查、3项CTest（含真实sender+fake MPI的参数冲突、
setter失败、静默读回不符、资源回收）通过；launcher9项回归通过。
本机测量工具5项单元测试通过，覆盖码流回退拒绝、真实slice位置、GOP/key AU与线占用预算。
原始USB日志/CSV/码流、探针及二进制ignore，仅源码与摘要入Git。

没有遍历全部QP值、Main profile或所有组合；未实现新渐进slice传输协议、未重开双路wrap，
未测无线/动态画质/长时间稳定性/光学延迟。下一步可先对两个候选做用户画质比较，再决定固化。
