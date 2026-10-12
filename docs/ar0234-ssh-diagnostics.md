# AR0234 接线假设、板级配置和 SSH 诊断

## 可验证的初始配置

以现有模组图纸、720p INI 和 Aura 官方原理图为依据，新增独立的单路板级配置：

| 项目 | 初始配置 / 依据 |
|---|---|
| 接口 | CSI0 / H3，实际针序由用户核对 |
| 控制器 | I²C4，100kHz；Linux adapter 编号由运行中的 DT 解析 |
| I²C 7-bit 地址 | **候选 `0x10`**，参考模组惯例，商家文件未确认 |
| 时钟 | 描述模组自带 24MHz 晶振；由 INI 的 XMCLK 和外部 MCLK 未引出推断 |
| 供电 | 模组 pin 1 的整板 3.3V，`vdd-supply = <&vcc3v3_sys>` |
| 内部电源 | 假设模块内部生成芯片 AVDD/DOVDD/DVDD；不伪造主板三路电源 |
| reset / PWDN | 不连接主板 GPIO；模组 pin 5/6 为 NC，假设内部上电复位 |
| MIPI | data lanes `<1 2>`、clock lane 0、连续 clock、450MHz link |
| 默认采集模式 | 1280×720 RAW10，按参考时序约 99.23fps |
| 第二路 | 暂时禁用，先验证单路 |

地址、自带晶振、内部电源/复位仍是实验假设，不能因为 DTB 编译通过就当作
硬件事实。读 chip ID 验证通信；后续帧率、CSI 错误和图像边界验证时钟/采集。
I²C 连接器的电平兼容性仍需结合实物确认，软件不能检测过压。

新增 SDK 文件：

```text
sysdrv/source/kernel/arch/arm64/boot/dts/rockchip/rv1126b-luckfox-aura-ar0234.dts
sysdrv/source/kernel/arch/arm64/boot/dts/rockchip/rv1126b-luckfox-aura-ar0234-csi0.dtsi
project/cfg/BoardConfig_Aura/BoardConfig-EMMC-RK801-Luckfox_Aura-Teleop-AR0234-Buildroot.mk
```

驱动的 DECXIN 配置使用 `vdd-supply` 表示模组整板供电；通用 sensor 配置
继续使用原来的 `avdd/dovdd/dvdd`。固定时钟节点只描述晶振，不产生物理 MCLK。
新 DTS 删除旧 IMX415/MIA1321 sensor 节点及其接收 endpoint，避免同端口竞争。
保留原板的网络、eMMC 和 SSH 配置。

## 一：旧镜像上也能先测 I²C

在本机工作区执行，脚本经 SSH 在 Aura 上运行，无需先复制：

```sh
cd /home/ww/codings/luckfox-aura-teleop

# 枚举：DT、I²C adapter/client、media graph、视频节点、内核日志。
ssh root@192.168.1.10 'sh -s --' < tools/diagnose-ar0234

# 读取候选地址 0x10 的芯片 ID 寄存器 0x3000。
ssh root@192.168.1.10 'sh -s -- --probe' < tools/diagnose-ar0234
```

默认脚本只枚举，不扫描所有 I²C 地址，不启流，不修改 GPIO 或传感器配置。
`--probe` 只访问一个地址的 chip-ID 寄存器。未绑定地址使用组合 I²C_RDWR 读：

```text
i2ctransfer -y <adapter> w2@0x10 0x30 0x00 r2
```

这里写入的两个字节是 16-bit 寄存器指针，后接两字节读；不是寄存器值写入。
不使用 SMBus byte/word 读去猜测这个传感器的地址格式。
预期返回 `0x0a 0x56`（Bayer）或 `0x1a 0x56`（mono）。

脚本先从 DT alias `i2c4` 和 sysfs adapter 的 `of_node` 对应关系找实际编号；
不能解析时停止访问，而不是直接猜 `/dev/i2c-4`。手动核对后可覆盖：

```sh
ssh root@192.168.1.10 'sh -s -- --probe --bus 4 --address 0x10' \
  < tools/diagnose-ar0234

# 若移到 CSI1，使用对应 alias i2c3。
ssh root@192.168.1.10 'sh -s -- --camera 1 --probe' < tools/diagnose-ar0234
```

如果实际地址有其它依据，可用 `--address 0xNN` 单独测试。
不同地址成功后，正式 DTS 中的节点 unit-address 与 `reg` 需要一起改。
目前没有添加全总线地址扫描，避免在其它器件上发送传感器寄存器指针。

旧镜像中不存在 AR0234 驱动不妨碍在未占用地址读 chip ID，前提是模块已供电、
摄像头 I²C adapter 已启用且存在 `/dev/i2c-N`。
当前 SDK staging 中已有 i2c-tools，本轮又在 teleop defconfig 中显式启用，
确保以后重建时保留 `i2ctransfer`；内核片段显式保留 `CONFIG_I2C_CHARDEV=y`。

## 二：驱动绑定后的 ID 与枚举

新驱动增加只读 sysfs `chip_id`，读取时由驱动获取锁与 runtime PM 引用，
实际读取 `0x3000` 后释放引用。因此驱动已经绑定时不需要 `i2ctransfer -f`。
脚本自动选择这个接口。直接读取示例（adapter/地址以实际枚举为准）：

```sh
cat /sys/bus/i2c/devices/4-0010/chip_id
```

如果地址被其它驱动占用，脚本不解绑、不强制访问，返回 unavailable。
若已绑定的 AR0234 驱动来自较早版本、没有 `chip_id`，同样返回 unavailable。

结果含义：

| 结果 | 下一步 |
|---|---|
| `PASS: AR0234 ... ID` | 通信和芯片身份已验证；继续看 sensor/media graph |
| 传输 NACK / timeout | 核对地址、供电、排线、电平和晶振，不先改 PLL |
| 返回其它 ID | 不写初始化表；确认地址上实际器件和读法 |
| ID 正确但没有 media sensor | 检查已烧镜像的 DTS、compatible、reg、驱动和 probe 日志 |
| media sensor 有、没有 ISP mainpath | 检查 DPHY/CSI/CIF/ISP 的双向 endpoint 与 status |

## 三：选择实验板配置并构建

2026-10-12 已切换 `.BoardConfig.mk` 到 AR0234，并在 Ubuntu Distrobox 完成
`./build.sh all`，生成整套分区镜像；尚未烧录或验证真实摄像头。
镜像内容检查与烧录注意事项见 [镜像构建记录](ar0234-image-build.md)。
重新选择和构建可执行：

```sh
./tools/select-teleop-board ../aura-sdk ar0234
distrobox enter aura-sdk -- bash -lc \
  'cd /home/ww/codings/aura-sdk && ./build.sh'
```

完整构建会带上内核、DTB、Buildroot 和其余匹配的组件。
需要烧录时由用户执行已存在的 `tools/flash-aura-emmc`；此诊断流程不会自行烧录。
切回原 MIA1321 板配置：

```sh
./tools/select-teleop-board ../aura-sdk mia1321
```

## 四：确认枚举后采集 120 帧

AR0234 驱动成功绑定、新设备树媒体链路完整，并且 sender/RKIPC 未占用节点后：

```sh
ssh root@192.168.1.10 \
  'sh -s -- --capture --frames 120 --timeout 10 --output /userdata/ar0234-test' \
  < tools/diagnose-ar0234
```

`--capture` 会启流、设置 ISP 输出格式，属于显式采集动作。
它先验证 chip ID，再找与所选 sensor 相连的 `rkisp-vir0` media graph，动态查询
`rkisp_mainpath` 节点，不写死 video13/video24。若 sensor/CIF 与 ISP 分属两个
media graph，则检查启用的 CIF→ISP bridge 和实际设备树中互指的 endpoint。
随后验证 ISP 确实接受
NV12 1280×720，再以 MMAP 采集到 `/dev/null`。不停止其它程序，不自动解绑驱动。

成功表示收到 120 帧；实际 fps、约 10.08ms 帧间隔、CSI 错误和图像质量仍要检查。
报告包含全部输出、采集日志和采集后的 dmesg，默认写到独立的
`/tmp/ar0234-report.*`，也可通过 `--output` 放到持久目录。
退出码：0 成功，1 ID/采集失败，2 参数错误，3 工具/设备/驱动接口未就绪。

## 本轮验证

Ubuntu 22.04 Distrobox 内：

- 驱动 6 项测试：INI 顺序、模式时序、启停/错误清理、chip-ID runtime PM 平衡、
  CIF 裁剪边界使用输出坐标且保留原生阵列裁剪。
- 脚本 12 项隔离测试：枚举无 I²C 事务、alias 映射、两类 ID、驱动占用保护、
  错误响应、自动节点解析、短流及格式不符拒绝、参数验证；分离 CIF/ISP graph
  连接验证及错误 bridge 拒绝。
- 编译后的 DTB 4 项检查：供电/时钟/地址、两条 lane 与互指 endpoint、旧 sensor
  删除/第二路禁用、主 Ethernet 和 eMMC 保持启用。
- AArch64 `.o/.ko` 编译与链接通过，`W=1 + -Werror`，DTB 编译通过。

统一复现：

```sh
distrobox enter aura-sdk -- bash -lc \
  'cd /home/ww/codings/luckfox-aura-teleop && ./tools/test-ar0234-driver-build'
```

上述自动化测试不执行真实 I²C 事务或采集。
2026-10-12 已通过 SSH 实机确认 `4-0010`、Bayer ID `0x0a56` 和 media 枚举，
并发现 CIF 裁剪边界兼容问题；详见 [实机测试记录](ar0234-hardware-test.md)。
构建状态见 [镜像构建记录](ar0234-image-build.md)。
