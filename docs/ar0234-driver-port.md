# AR0234 / DECXIN-3478V1 驱动移植记录

2026-10-12 更新：新增 `decxin,3478v1` 专用的商家 720p 两通道配置，
使用连续 MIPI 时钟，默认曝光约 7ms。下文描述初始通用参考配置；
当前专用配置、设备树要求与测试结果见 [720p 接入记录](ar0234-720p-bringup.md)。
Aura 的引脚表与商家同号匹配；实际排线方向确认见 [接线核对记录](ar0234-wiring-confirmation.md)。

## 当前状态（2026-10-11）

本轮把 AR0234 传感器驱动接入 Aura 的 Rockchip Linux 6.1.141 SDK。
驱动代码、Kconfig、Makefile 和 teleop 配置片段随 kernel patch stack 保存。
现有 MIA1321 设备树没有修改；尚未启用任何 AR0234 节点。
目前只完成编译和模块链接验证，未验证芯片探测、RAW 采集、ISP 或 MPP 出图。

商家提供的模组型号为 DECXIN-3478V1，图纸标注 AR0234、22P/0.5mm
下接触连接器，引出四组 MIPI 数据差分对和一组时钟差分对。
仅凭连接器规格不能确认排线和 Aura 的电气兼容性，需核对接触面和编号方向。
图纸模组 pin 1 是 3.3V，而常见树莓派 22-pin 表的 pin 22 是 3.3V。

## 代码来源及适配

- 寄存器、PLL、裁剪和 binning 表来自
  [Kurokesu/ar0234-rpi-driver](https://github.com/Kurokesu/ar0234-rpi-driver)，
  固定提交 `eefdf9c42cb8f06f7cf00a18b08c7c23d9363a9f`，GPL-2.0。
  源码保留 Raspberry Pi、Kurokesu 的版权与作者信息。
- Rockchip subdev 注册、模块命名、模块信息等接口参考 SDK 的
  `drivers/media/i2c/mia1321.c`，保留对应来源说明。
- 使用原生 I²C 传输处理 16-bit 寄存器地址及 8/16-bit 大端数据，
  不依赖本 BSP 缺少的 `v4l2-cci.h`。
- 将新式 stream/state API 改成 Linux 6.1 的 `s_stream`、TRY/ACTIVE format、
  帧率和 mbus 配置回调；每个实例独立持有锁、模式、控件和电源状态。
- 实现 `RKMODULE_GET_MODULE_INFO`、线性 HDR 配置查询、
  `RKMODULE_GET_CHANNEL_INFO`、quick-stream 和 32-bit compat ioctl。
  quick-stream 走完整状态机和电源管理，不绕过初始化。
- 仅暴露一个 RAW 图像 pad。参考 Linux-media AR0234 v5 补丁及本 SDK
  AR0230 的设置，将 `SMIA_TEST` 配为 `0x1802`，关闭附加数据/统计输出。
  这个寄存器及帧开销仍需对照商家规格书并实测。

SDK 文件路径：

```text
sysdrv/source/kernel/drivers/media/i2c/ar0234.c
sysdrv/source/kernel/drivers/media/i2c/Kconfig
sysdrv/source/kernel/drivers/media/i2c/Makefile
sysdrv/source/kernel/arch/arm64/configs/rv1126b_teleop_low_latency.config
```

配置片段添加 `CONFIG_VIDEO_AR0234=y`；缺少启用的设备树节点时不会探测模组。
以后正常 SDK kernel 构建会将驱动编入内核，本轮没有生成新烧录镜像。

## 已接入的功能与参数边界

- 自由运行的 2-lane 或 4-lane CSI-2，顺序映射 `1 2` 或 `1 2 3 4`，
  clock lane 0、不反相、非连续时钟。
- 仅采用参考驱动的 24 MHz 输入时钟表。时钟名称支持 Rockchip `xvclk`
  或参考驱动 `extclk`。其它频率明确拒绝，不猜测新 PLL 表。
- endpoint 必须明确给出一个 link frequency：450 MHz 对应 RAW10，
  360 MHz 对应 RAW8。这里是 DDR link frequency，lane 速率为它的两倍。
  原始 bpp 由该配置固定，不能通过 format ioctl 随意切换。
- 模式：1920×1200、1920×1080、1280×720 裁剪、960×600 2×2 binning。
  枚举帧率按 lane 数和实际配置计算；可通过 VBLANK / frame interval 降帧率。
  参考表默认选择最大帧率，不表示 DECXIN 模组已验证该帧率。
- 芯片 ID 接受参考驱动的 `0x0a56`（Bayer）和 `0x1a56`（mono），
  在上电后读取，不跳过识别。实际模组版本仍待确认。
- 曝光、模拟/数字增益、测试图案、水平/垂直翻转。
  翻转同步更新 Bayer 格式，采集期间锁定翻转和模式切换。
- 低延迟测试的默认曝光先设为 100 行，可由 V4L2 调整；曝光单位是行，
  不是微秒。最大值随 VBLANK/模式变化，避免超过一帧。
- 采用 runtime PM；reset/pwdn GPIO 的逻辑 1 表示断言，0 表示释放，
  物理高低电平由 DTS 的 `GPIO_ACTIVE_LOW/HIGH` 决定。
- 电源名采用 Rockchip `avdd`、`dovdd`、`dvdd`。电压、是否由模块内部稳压、
  是否需要主板控制电源，不由驱动猜测，等待规格书后映射到正确 regulator。
- 外部触发、双目同步和闪光输出本轮未移植。
  DTS 若给出 `trigger-mode` 或 `flash` 会明确报错，防止误以为已经支持。

### Rockchip 时序报告

参考驱动内部行计数为 612 ticks，读出 clock 为 45 MHz（2-lane）或
90 MHz（4-lane），并包含参考值 5 行的帧开销。
为了向 Rockchip 报告非负 HBLANK 和标准 CSI pixel rate，二者同时乘 4：
HTS=2448，pixel rate=180/360 Mpixel/s；时间比例保持不变。
寄存器仍写入行长 612，帧长寄存器为 `height + vblank - 5`。
`g_frame_interval` 报告 `2448 * (height + vblank) / pixel_rate`。
`s_frame_interval` 使用完整 64-bit 除法，支持这个未约分的分数回传。
这些是参考驱动时序模型，需以实机时间戳校准。

## 设备树模板（尚未应用）

以下是 CSI0 的编辑模板，包含占位符，**不能原样放进 DTS 编译**。
先核实所有占位符，再替换当前 sensor endpoint 的连接关系。
CSI1 时使用实际第二路 I²C 和 DPHY endpoint，而非复制端口编号。

```dts
/* 如果模组自带晶振，这只描述晶振，不会使主板输出物理 MCLK。 */
/ {
    ar0234_osc: ar0234-osc {
        compatible = "fixed-clock";
        #clock-cells = <0>;
        clock-frequency = <24000000>; /* 待确认 */
    };
};

&i2c4 {
    ar0234_0: camera@I2C_ADDR {
        compatible = "onnn,ar0234cs";
        reg = <I2C_ADDR>; /* 参考模组为 0x10，商家模组尚未确认 */
        status = "disabled";
        clocks = <&ar0234_osc>;
        clock-names = "xvclk";
        reset-gpios = <RESET_GPIO_CONTROLLER RESET_PIN RESET_POLARITY>;
        /* 若有外部 PWDN，再加入 pwdn-gpios，不能从 NC 脚猜测。 */
        avdd-supply = <&VERIFIED_ANALOG_SUPPLY>;
        dovdd-supply = <&VERIFIED_IO_SUPPLY>;
        dvdd-supply = <&VERIFIED_CORE_SUPPLY>;
        rockchip,camera-module-index = <0>;
        rockchip,camera-module-facing = "back";
        rockchip,camera-module-name = "DECXIN-3478V1";
        rockchip,camera-module-lens-name = "VERIFIED_LENS_NAME";
        port {
            ar0234_0_out: endpoint {
                remote-endpoint = <&VERIFIED_DPHY_INPUT_ENDPOINT>;
                clock-lanes = <0>;
                data-lanes = <1 2 3 4>;
                clock-noncontinuous;
                link-frequencies = /bits/ 64 <450000000>;
            };
        };
    };
};
```

接收端也要改成四条 lane，remote-endpoint 指回 `ar0234_0_out`，并禁用同一
端口的旧 sensor 节点及其 endpoint。当前 SDK 有双 MIA1321 配置，不能仅加
新节点让两个 sensor 占用同一 DPHY。不要复制树莓派的 overlay 到 Rockchip。

## 编译检查及复现

在 CachyOS 工作区执行，编译发生在 Ubuntu 22.04 Distrobox：

```sh
cd /home/ww/codings/luckfox-aura-teleop
distrobox enter aura-sdk -- bash -lc \
  'cd /home/ww/codings/luckfox-aura-teleop && ./tools/test-ar0234-driver-build'
```

脚本默认定位相邻 `../aura-sdk`，也可以指定 `AURA_SDK_ROOT`。
它读取现有 SDK 的 kernel `.config` 和 `Module.symvers`，在忽略的 `build/`
目录生成独立内核准备目录；检查 builtin `.o` 和单驱动外部 `.ko` 的编译/链接。
模块产物仅作编译检查，不代表可加载到任意已有镜像。
`artifacts/`、`build/`、`logs/` 均已被 gitignore 排除。

已验证：GCC 12.4.0 / AArch64 / Linux 6.1.141 PREEMPT，`W=1`、`-Werror`
编译与 MODPOST/模块链接通过；checkpatch 无错误、无警告。

## 规格书到达后的验证顺序

1. 确認排线映射、供电、I/O 电平、晶振/MCLK、reset/PWDN、I²C 地址、
   芯片版本和 Bayer/mono；校对公共寄存器、PLL、MIPI 时序和帧开销。
2. 完成一个 CSI 端口的 DTS，先启用单路；构建镜像后由用户烧录。
3. 通过 SSH 查看 chip ID、`media-ctl -p`、link frequency 和 lane 数。
4. 使用测试图案采 RAW，确认字节数、格式、帧率以及无 CSI ECC/CRC/丢帧。
5. 再采真实 RAW，手工设置短曝光/增益；彩色版需准备匹配的 ISP IQ/3A。
6. 验证 ISP NV12 输出，再接 MPP sender；重新查询真实 video/VI 编号。
   现有 sender 的 1280×1080 默认值是 MIA1321 模式，新 sensor 需显式选
   分辨率和 fps，不能把编码器 fps 当成传感器 fps。
7. 单路稳定后再启用第二路及外部同步，最后重测 screen-to-screen 延迟。

参考：[Linux-media AR0234 v5 原始补丁](https://www.spinics.net/lists/linux-media/msg273780.html)、
[参考驱动固定提交](https://github.com/Kurokesu/ar0234-rpi-driver/tree/eefdf9c42cb8f06f7cf00a18b08c7c23d9363a9f)。
