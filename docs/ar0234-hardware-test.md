# AR0234 首次 SSH 实机测试

2026-10-12，用户烧录首次 AR0234 镜像、连接摄像头和网线后，通过
`root@192.168.1.10` 执行测试。没有自动烧录、重启或解绑传感器驱动。

## 实机已确认

- 内核：`6.1.141 #11 SMP PREEMPT Mon Oct 12 14:01:35 CST 2026`。
- DT model：`Luckfox Aura (DECXIN AR0234 720p bring-up)`。
- 网口：eth0 `192.168.1.10/24`，1 Gbps Full duplex，SSH 密码登录正常。
- 摄像头在 I²C4、7-bit 地址 `0x10`；`4-0010` 已绑定 `ar0234` 驱动。
- 开机 probe 和通过驱动/runtime PM 读取的芯片 ID 均为 `0x0a56`，Bayer。
- sensor：`m00_b_ar0234 4-0010`，`/dev/v4l-subdev3`。
- sensor 格式：SGRBG10、1280×720，报告帧间隔 `1813968/180000000`。
  这是驱动时序参数，不是成功采集测出的 fps，也不证明实体晶振频率。
- sensor/CIF graph：`/dev/media1`，rkcif-mipi-lvds。
- ISP graph：`/dev/media2`，rkisp-vir0；mainpath `/dev/video13`。
- 通过两端 sysfs 的 DT 节点和互指 endpoint 确认 CIF→ISP bridge。

这些结果验证了本次连接下的模组供电、I²C 通信和芯片身份。
尚未验证 MIPI 出帧、帧率、图像质量或 MPP 编码。

## 采集失败及原因

第一次 `--capture` 因诊断脚本假设 sensor 与 ISP 在同一个 graph 中，
提前返回 unavailable。修复后，脚本已在实机正确解析分离 graph 和 video13。

直接配置 ISP 为 SGRBG10 1280×720、mainpath 为 NV12 1280×720 后，
120 帧采集仍在 10 秒限时内没有收到帧。内核记录：

```text
rkcif-mipi-lvds: stream[0] start streaming
rkcif-mipi-lvds: crop size is bigger than input
rkisp-vir0: rkisp_stream_stop id:0 timeout
```

本地 BSP `cif/subdev-itf.c` 和 `cif/capture.c` 把 sensor 的
`V4L2_SEL_TGT_CROP_BOUNDS` 当作线上输入帧的裁剪区域。
移植驱动此前返回整个 1920×1200 阵列、偏移 (8,8)，但实际模式已经在 sensor
内部裁剪为 1280×720，导致 CIF 拒绝启流。修改 ISP 输出尺寸不能修复这个错误。

## 本轮修复

- kernel 提交 `6f1aee66d`：AR0234 的 CROP_BOUNDS 返回输出帧坐标
  `(0,0)/mode_width×mode_height`，保留 CROP 的 sensor 原生阵列坐标。
- SDK 补丁：`0015-media-ar0234-report-output-crop-bounds-to-Rockchip-C.patch`。
- 诊断脚本支持分离 CIF/ISP graph，校验启用的 bridge 与互指 DT endpoint。
- Ubuntu Distrobox 内 22 项测试通过（驱动 6、诊断 12、DTB 4），
  AArch64 驱动 `.o/.ko` 编译/链接通过，`W=1 + -Werror`，checkpatch 无告警。

完整修复镜像构建和核验结果见 [镜像记录](ar0234-image-build.md)。
新内核仍需由用户烧录；本次实机上的旧内核尚不能通过采集测试。

## 再次烧录后的测试

```sh
cd /home/ww/codings/luckfox-aura-teleop
ssh root@192.168.1.10 \
  'sh -s -- --capture --frames 120 --timeout 10 --output /userdata/ar0234-after-crop-fix' \
  < tools/diagnose-ar0234
```

先检查是否收到全部 120 帧，再检查约 10.08 ms 帧间隔、seq 丢帧和 CSI 错误。
如果仍失败，以新日志排查，不能把芯片 ID 成功当作完整视频链路成功。

本机完整日志保存在忽略目录 `logs/ar0234-hardware/`，板上报告保存在
`/userdata/ar0234-probe-first`、`/userdata/ar0234-capture-first`、
`/userdata/ar0234-capture-split-graph`。全量烧录会覆盖这些板上报告。
