# 固化已接受画质的双路低延迟编码配置

2026-10-15。用户确认候选画质可接受后，将上一轮优先推荐的
**GOP15、CAVLC、4×4变换、关闭slice拆分**设为AR0234双摄launcher默认。
两个候选的阶段均值之和约3.92/3.91ms、差异不足以判定速度优劣；本次选择保留帧间预测的GOP15。
此前计时见[参数实机结果](codec-parameter-results-2026-10-15.md)，本轮只做固化与集成验证，
没有新增光学或完整编解码延迟测量。

## 当前启动方式

板端直接执行：

```sh
/userdata/run-ar0234-stereo
```

默认双路1280×720、约119.95fps（VBLANK506、VENC声明120），曝光735=4.998ms、
模拟/数字增益48/512；High profile、CBR30000kbps/路、GOP15、CAVLC、仅4×4变换、
slice拆分关闭、wrap0、整帧AU立即发送。去块滤波、重编码次数继续沿用原SDK默认。
目的地址仍192.168.1.2:5000，viewer启动方式不变。

`tools/run-ar0234-stereo`把上述codec选项显式传给sender；通用sender裸命令的MIA1321默认不改。
环境变量仍可覆盖，`--dry-run`显示当前选择，不改sensor控件或启流。
逐slice诊断必须同时设置非零`SLICE_CTUS`，与关闭拆分冲突时在预检前拒绝。

回到上一轮旧编码参数：

```sh
ENTROPY=cabac TRANSFORM=auto SLICE_CTUS=120 /userdata/run-ar0234-stereo
```

若想试此前计时的GOP1+CAVLC+不拆分候选，还需要恢复自动变换选择，才能与该组一致：

```sh
GOP=1 TRANSFORM=auto /userdata/run-ar0234-stereo
```

## 构建、部署与验证

Ubuntu22.04 Distrobox中通过默认`./tools/build-aura`重建`build/aura/bin/aura-sender`，
runtime依赖检查与launcher9项回归通过。正式`/userdata/teleop-sender`已更新为支持codec参数的版本，
不再需要`SENDER=/userdata/teleop-sender-codec`；实验文件可继续用于对照。
正常sender SHA256：`01e55734e2a755c649e61ba13a873cbf31bb93f37b5d72c9036cd48e8a3eb946`。

旧程序与脚本备份到ignored `artifacts/codec-defaults-20261015/`；新文件先上传临时名再替换。
没有修改内核/SDK/DTS，没有刷写、重启或新增开机自启。
以后本仓库构建和`tools/deploy-aura`会部署对应程序及默认脚本；旧镜像不会自行更新它们，
全量刷写覆盖userdata后仍需重新部署。

清除所有相机/codec/SENDER覆盖变量后，以正常launcher各120帧实测；仅PORT5500避开日常端口，
CODEC_REPORT/TELEOP_METRICS用于观测。实际MPI读回两路profile100、CBR、GOP15、
entropy0、transform1、split_enabled0。原生CachyOS接收240 AU，240帧全部VAAPI硬解，
malformed/duplicates/dropped0、每路队列峰值1、溢出0、退出无积压。
独立USB日志活标记后无新内核/CSI告警，boot ID未变，结束温度48.91°C；
读取两颗sensor确认735/48/512/506仍生效，测试程序已退出。

[固化验证摘要](baselines/2026-10-15-codec-defaults.json)。原始日志、备份和构建产物保持ignore。
