# DECXIN-3478V1：720p 两通道模式接入

后续已新增完整的实验 DTS、整板 `vdd-supply`、驱动只读 chip ID 接口和
SSH 诊断脚本，详见 [板级配置与 SSH 诊断](ar0234-ssh-diagnostics.md)。
下面保留本轮初始化表接入时的记录。

## 本轮结果（2026-10-12）

SDK 驱动新增模组专用配置：使用 `compatible = "decxin,3478v1"` 的节点，
默认仅枚举和使用商家提供的 **1280×720、2-lane、RAW10、约 99.23fps** 模式。
通用 `onnn,ar0234cs` 配置保留原有四种参考模式，不通过全局 module 参数
选择配置，两个 sensor 实例可以分别使用各自的配置。

商家 INI 的 `[720P 99.2fps]` 被转换成独立的
`drivers/media/i2c/ar0234-decxin-3478v1.h`，并由 `ar0234.c` 调用。
161 次寄存器写入、重复地址、镜头 POLY 校正和 5 个延时保持原顺序。
`DELAY` 按毫秒解释，总计 920ms；首次启动等待不代表稳定采集的帧间隔。

实际参数：

| 参数 | 值 |
|---|---|
| 输入时钟 | 24MHz |
| MIPI data lanes | `<1 2>` |
| link frequency | 450MHz，即每 lane 900Mbit/s |
| MIPI clock | 连续；INI `0x31BC=0x8D89` |
| 裁剪区域 | x=328、y=248、1280×720 |
| 行长寄存器 `0x300C` | 612 |
| 帧长寄存器 `0x300A` | 736 |
| 默认曝光 `0x3012` | 514 行，按参考时序约 6.99ms |
| 默认模拟增益 `0x3060` | `0x000d` |
| 默认 VBLANK | 21，计入参考模型的 5 行开销 |
| 报告 HTS / pixel rate | 2448 / 180Mpixel/s |
| 报告帧率 | `180000000 / (2448 * 741)`，约 99.22998fps |

### 驱动与 INI 的衔接

专用配置不会混入参考驱动的 PLL、公共模拟寄存器或 binning 初始化表。
停止采集和异常清理按商家使用的 RESET_REGISTER `0x301A=0x2058` 执行；
重新开启采集重新执行完整 INI 序列。probe 阶段只做识别，不执行商家启停序列。

为了与 Rockchip 的单 RAW pad 管线衔接，驱动在 INI 之前额外写
`SMIA_TEST 0x3064=0x1802`，关闭附加数据/统计输出。这是驱动的明确附加设置，
商家 INI 未提供该值，实机需核对图像边界和 CSI 错误。

完成 INI 后应用已缓存的 V4L2 控件，包括曝光、增益、翻转、VBLANK 和测试图案。
默认曝光与模拟增益与商家值一致；数字增益等仍使用通用驱动的默认值。
用户在启流前设置的控件值会覆盖默认值。默认曝光暂不缩短，便于与商家基线对照。

模式、行长、帧长默认值和曝光默认值由模式对象管理。
配置只接受两条顺序数据通道、450MHz link frequency 和连续 clock；
四条 lane、RAW8 或 `clock-noncontinuous` 会明确拒绝。
帧率请求超过约 99.23fps 会钳制到本模式上限，返回实际帧间隔；可通过 VBLANK 降帧率。

## 后续设备树选择

现有 MIA1321 设备树没有改动，尚未生成本模组的新烧录镜像。
确认排线、I²C 7-bit 地址、板载晶振和供电/复位电路后，再填写 sensor 节点。
以下仅展示已经确认的驱动配置选择，省略未知的 reg、GPIO、regulator、clock
和 remote-endpoint，不能作为完整 DTS 使用：

```dts
compatible = "decxin,3478v1", "onnn,ar0234cs";
/* 确认硬件后再设置 status = "okay"。 */
status = "disabled";

port {
    sensor_out: endpoint {
        clock-lanes = <0>;
        data-lanes = <1 2>;
        link-frequencies = /bits/ 64 <450000000>;
        /* 不添加 clock-noncontinuous；继承旧节点时需要删除该属性。 */
    };
};
```

接收 DPHY endpoint 同样配置两条 lane 并指回 sensor endpoint，禁用同端口
旧 sensor；不能把旧节点的 lane 数、时钟和 GPIO 设置原样套用。
`clock-noncontinuous` 若从旧节点继承，使用 `/delete-property/ clock-noncontinuous;`。
节点继续使用 Rockchip 模块信息字段；时钟名称为 `xvclk` 或 `extclk`，
实际供应方式应按板载晶振/MCLK 电路确定，而不是从 INI 的 `XMCLK` 猜测。

## 验证与复现

```sh
cd /home/ww/codings/luckfox-aura-teleop
distrobox enter aura-sdk -- bash -lc \
  'cd /home/ww/codings/luckfox-aura-teleop && ./tools/test-ar0234-driver-build'
```

在 Ubuntu 22.04 Distrobox 中验证：

- 从当前驱动提取实际初始化/启停/时序函数，以模拟 I²C、延时和 PM 执行；
  对照原始 INI 验证全部写入与延时顺序。
- 验证默认分辨率、帧长、行长、曝光、帧率分数及原参考模式的时序保留。
- 注入初始化中不同位置的 I²C 错误和控件应用错误，验证返回错误、停止采集、
  不继续执行后续表以及释放 PM 引用；重复启流不重复初始化。
- `W=1 + -Werror` 编译 builtin AArch64 对象并链接外部 `.ko`，MODPOST 通过。
- 两个源码文件的 checkpatch 无错误、无警告。

所有生成的 C harness、二进制、模块、日志都放在被忽略的 build/logs 目录。
这些测试验证软件行为和 BSP API；没有测试真实传感器、I²C、电源时序或 CSI 出图。

## 首次实机出图后

先确认 sensor 与 ISP 输出均为 1280×720、帧间隔约 10.08ms、没有持续 CSI
ECC/CRC 错误。再启动 sender，并按实际 media graph 选择 VI device/pipe：

```sh
/userdata/teleop-sender --camera 0 \
  --width 1280 --height 720 \
  --max-width 1280 --max-height 720 \
  --fps 100 --bitrate-kbps 20000 --gop 15 \
  --wrap-lines 256 --slice-ctus 120 --frames 0
```

此命令是假定该模组接在 camera 0 上的编码测试配置；`--fps 100` 是编码器整数
帧率声明，不会把传感器的约 99.23fps 变成精确 100fps。
只有 RAW/ISP 采集验证通过后，才能判断 MPP 链路问题。
亮度足够时，再使用曝光控件缩短曝光；100 行在本模式下约为 1.36ms。

原始资料及未确认项目见 [商家资料核对](ar0234-vendor-files-review.md)，
通用驱动记录见 [初始移植记录](ar0234-driver-port.md)。
