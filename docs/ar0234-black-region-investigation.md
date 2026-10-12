# AR0234 画面右侧发黑：实机排查

2026-10-12，针对“左侧约 1/4 正常、右侧约 3/4 黑屏”进行 SSH 测试。
本次场景下确认为欠曝光造成的暗区，而非右侧图像传输缺失。
提高曝光和增益后，右侧墙面、线缆、设备均可见，用户确认实时 viewer 看清了。
本轮没有修改 SDK、驱动、设备树或 sender/viewer 源码，也没有烧录或重启。

## 定位证据

1. 先检查此前 ISP 直接保存的 NV12 和 MPP H.264 解码图：两者相同位置都暗，
   因而问题已经出现在编码前。
2. 将 AR0234 `test_pattern=2` 临时设为 Vertical Color Bars，经 MIPI/CIF/ISP
   保存完整 1280×720 NV12，右侧彩条正常。
3. 相同彩条经过 MPP、UDP、VAAPI、SDL3/Vulkan：300 帧发送、解码和显示一致，
   malformed/duplicates/dropped 全部为 0，文件解码 warning 日志为空。
   用户确认彩条显示正常、镜头无遮挡。
4. 在同一实际场景中，仅通过 V4L2 控制修改曝光与增益，右侧原来的暗区出现
   可辨认的场景内容。实时编码和显示再次验证，用户确认画面看清了。

色条验证覆盖整幅图像传输，但不验证实际像素读出的曝光质量；
真实场景对比才是本轮欠曝光结论的依据。

## 图片位置

本机目录：`/home/ww/codings/luckfox-aura-teleop/captures/ar0234/black-region/`。
原始 NV12、H.264 和 PNG 均保留在这个目录，受 `.gitignore` 排除，不提交采集数据。

| 文件 | 含义 |
|---|---|
| `scene-default.png` | 默认 exposure=514、analogue_gain=13、digital_gain=128，ISP 直接采样 |
| `scene-bright.png` | exposure=735、analogue_gain=32、digital_gain=512，ISP 直接采样 |
| `scene-bright-h264.png` | 同一亮度参数经 MPP 编码，再在本机解码得到的第 40 帧 |
| `color-bars-nv12.png` | 内置彩条经过 ISP 的图像 |
| `color-bars-h264.png` | 内置彩条经过 MPP 的解码图像 |

可以在本机执行：

```sh
xdg-open /home/ww/codings/luckfox-aura-teleop/captures/ar0234/black-region/scene-default.png
xdg-open /home/ww/codings/luckfox-aura-teleop/captures/ar0234/black-region/scene-bright.png
```

对未压缩 NV12 的 Y 平面按横向四等分统计，平均值如下：

| 参数 | 左 1/4 | 第二 1/4 | 第三 1/4 | 右 1/4 |
|---|---:|---:|---:|---:|
| 原参数 | 52.586 | 14.052 | 12.926 | 11.959 |
| 提高曝光/增益 | 179.821 | 58.702 | 42.909 | 28.878 |

原图右侧不是空缓冲区；其像素值接近黑色，而增加曝光/增益后亮度和细节都有响应。
左右亮度差也与该场景左侧的强光源有关。偏绿、噪声和强光源过曝仍可见，
这组参数用于室内验证，不是完成 ISP 白平衡、降噪和动态范围调校后的配置。

## 本次运行时配置

当前板上 sensor 节点是 `/dev/v4l-subdev3`（`m00_b_ar0234 4-0010`）。
本轮结束时保留如下运行时设置：

```text
exposure: 735
analogue_gain: 32
digital_gain: 512
vertical_blanking: 21
test_pattern: 0
```

曝光单位为 sensor 行；依据当前 HTS/pixel rate，735 行约 10.00 ms，
原默认 514 行约 6.99 ms。这会增加运动模糊和积分等待，需要与遥操作延迟目标
一起权衡。垂直消隐、PLL、lane 和帧率配置没有改动；后续可优先降低曝光，
通过改善照明和选择合适增益维持可见度。

重启会恢复驱动默认控制值。重新应用并运行 sender：

```sh
# 在板上执行；节点编号变化时先用 media-ctl 查询实际 sensor 节点。
v4l2-ctl -d /dev/v4l-subdev3 \
  --set-ctrl=exposure=735,analogue_gain=32,digital_gain=512,test_pattern=0
/userdata/teleop-sender --camera 0 --width 1280 --height 720 --fps 100 --frames 0
```

恢复本次测试前的商家默认值：

```sh
v4l2-ctl -d /dev/v4l-subdev3 \
  --set-ctrl=exposure=514,analogue_gain=13,digital_gain=128,test_pattern=0
```

## 实时亮度配置验收

```text
aura-sender: encoded-frames=300 datagrams=9559 bytes=11313126
viewer: access_units=300 datagrams=9559 bytes=11313126 malformed=0 duplicates=0 dropped=0
viewer stream 0: decoder: frames=300 hardware_frames=300
presenter: frames=300
```

保存的 H.264 全文件解码 warning 日志为空。测试进程已退出，没有留下持续运行
的 sender/viewer。完整日志与亮度统计保存在忽略目录
`logs/ar0234-hardware/black-region/`。
