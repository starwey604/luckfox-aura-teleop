# AR0234 首次 SSH 实机测试

当前状态：复刷 CIF 裁剪修复镜像后，采集、MPP 编码、UDP、VAAPI 和 Vulkan
显示链路已运行；用户观察到右侧约 3/4 黑屏，**图像完整性尚未通过**。
详见文末的复刷测试记录。

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

## 复刷裁剪修复镜像后的测试

用户再次烧录后，SSH 确认运行 `6.1.141 #12 SMP PREEMPT`，构建时间
2026-10-12 14:16:35。sensor/CIF/ISP 均自动显示 1280×720，裁剪偏移 (0,0)，
`crop size is bigger than input` 错误消失。

### 实机采集

- 诊断脚本采集 120 帧成功，NV12，每帧 1,382,400 bytes。
- 进一步采集 1,200 帧成功；初始化阶段 seq 从 2 跳到 26，缺口 23 帧。
- 略过前 40 个返回帧后的 1,160 帧，实测 99.23244 fps，平均间隔
  10.07735 ms，最小 10.036 ms、最大 10.122 ms，无后续 seq 缺口。
- DPHY 日志：900 Mbps/lane。采集期间没有发现 CSI CRC/ECC/ERR1/ERR2 错误。
- 仍有 online ISP 消隐警告：要求 vblank ≥1,000 µs，当前约 286 µs。
  本轮保持商家初始化表和默认时序，没有为消除警告改动帧率。
- 抓取了跳过 40 帧后的 NV12 样本，本机预览为较暗、偏绿的场景。
  用户随后指出左侧约 1/4 正常，右侧约 3/4 黑屏，需单独排查。

初始化表本身含 stream-on → 200 ms 等待 → stream-off → 50 ms 等待 →
stream-on → 50 ms 等待；这可能与启流阶段的缺口有关，但本轮没有进一步
验证因果。不能据此认定初始化期间零丢帧。

### MPP → UDP → VAAPI → Vulkan

sender 已部署到 `/userdata/teleop-sender`。测试使用独立 UDP 端口 5002，
wrap 256 lines、slice 120 CTUs、H.264 30 Mbps，VENC rate-control 参数为
100 fps；sensor 实测仍为上述约 99.23 fps。

首次保存有限帧数 H.264 时，发现请求 600 帧却发送 601 帧，FFmpeg 对尾帧
报告 `corrupt decoded frame`。原因是 sender 通过下一个 PTS 确认前一帧完整，
停止时却把尚未确认完整的下一帧片段也 flush 了。

本轮修复 sender：达到帧数上限后停止收集下一帧，并在退出时丢弃尚未通过
下一 PTS 确认完整的片段。不会修改 SDK 或传感器寄存器，无需再刷镜像。
在 Ubuntu Distrobox 交叉编译和 runtime 检查通过，已重新部署并复测：

```text
aura-sender: encoded-frames=300 datagrams=474 bytes=369238
viewer: access_units=300 datagrams=474 bytes=369238 malformed=0 duplicates=0 dropped=0
viewer stream 0: decoder: frames=300 hardware_frames=300
presenter: frames=300
```

- 本机 viewer 在 CachyOS 原生运行，使用 VAAPI 硬件解码、SDL3/Vulkan 显示。
- 同一份 H.264 文件经 ffprobe 确认为 1280×720、恰好 300 帧；
  FFmpeg 全文件解码的 warning 日志为空。
- `--frames 0` 运行后发送 SIGINT 退出，保存 289 个完整帧；
  ffprobe 帧数一致，FFmpeg 解码 warning 日志为空，验证中断退出不发送残缺尾帧。
- 测试程序均已退出，板上没有遗留 sender/采集任务。

这证明数据链路及有限帧数/中断退出正常，**不证明图像内容完整**，
也不是新的 screen-to-screen 延迟测量。右侧黑屏按用户要求留待下一轮修复。

### 手动运行参考

本机：

```sh
./build/host/bin/host-viewer --port 5000 --frames 0 --timeout-ms 3600000
```

板上（当前 sender 的默认高度仍是旧 MIA 模组的 1080，AR0234 必须指定 720）：

```sh
/userdata/teleop-sender --camera 0 --width 1280 --height 720 --fps 100 --frames 0
```

上述启动命令留供后续验证；本轮没有留下持续运行的进程。
本机证据：`logs/ar0234-hardware/after-crop-fix-capture.log`、
`capture-statistics.json`、`mpp-final-viewer.log`、`mpp-final-sender.log`、
`mpp-final-file-decode.log`、`mpp-interrupt-file-decode.log`。
采集样本在忽略目录 `captures/ar0234/`。
