# AR0234 镜像构建与烧录记录

当前最新镜像为当天 14:16–14:17 生成的 CIF 裁剪修复版，见文末。
下面 14:01–14:02 的记录保留为首次构建历史。

2026-10-12，在 Ubuntu 22.04 `aura-sdk` Distrobox 中执行 `./build.sh all`，
退出码 0，`build_all` 和 `build_firmware` 均成功。镜像位于
`/home/ww/codings/aura-sdk/output/image`，生成时间为当天 14:01–14:02。
本次没有访问烧录设备、烧录镜像或重启开发板。

## 使用的配置

- BoardConfig：`BoardConfig-EMMC-RK801-Luckfox_Aura-Teleop-AR0234-Buildroot.mk`。
- DTS：`rv1126b-luckfox-aura-ar0234.dts`，CSI0 单路 AR0234，CSI1 禁用。
- 摄像头模式：1280×720 RAW10、2-lane、连续时钟、默认约 99.23 fps。
- 模组候选参数：I²C4、地址 `0x10`、24 MHz 内置晶振、3.3 V 模组供电。
  地址和晶振仍需用实物验证；编译通过不代表已经收到图像。
- 内核：`CONFIG_VIDEO_AR0234=y`、`CONFIG_I2C_CHARDEV=y`、
  `CONFIG_PREEMPT=y`、`CONFIG_HZ_1000=y`。

源码提交：kernel `17379bd8b`、sysdrv `ad25269`、BoardConfig `6158a8e`。
构建日志保存在忽略目录 `logs/ar0234-full-image-build.log`。

## 已检查的镜像内容

- 从最终 `boot.img` 提取 DTB，在 Distrobox 中执行四项设备树测试，全部通过：
  地址/供电/时钟、两条 lane 和互指 endpoint、旧 sensor 删除/第二 ISP 禁用、
  Ethernet/eMMC 保留。
- 从 `boot.img` 提取压缩内核，与此次构建的 `Image.lz4` 逐字节一致。
- 从 `oem.img` 提取 kmpp、kmpp_smart、rockit、rockit_base、rockit_osal，
  vermagic 均为 `6.1.141 SMP preempt mod_unload aarch64`。
- 直接检查 `rootfs.img`：SSH 开机脚本、root 密码登录配置、
  `eth0=192.168.1.10/24` 的 dhcpcd 配置、v4l2-ctl、media-ctl、i2ctransfer 均存在；
  root 密码验证为 `luckfox`。
- 当前 sender 也在 Distrobox 重新交叉编译，`tools/check-aura-runtime` 通过。
  二进制位于 `build/aura/bin/aura-sender`，需要另行部署到 `/userdata`。
- `tools/flash-aura-emmc --dry-run` 通过，未访问 USB 设备。

主要镜像 SHA-256（完整列表在忽略目录 `artifacts/ar0234/image-verification/SHA256SUMS`）：

```text
4ae3da7c94e6d013cc67df9d1c6e9d25e8cf27de523fa94600551df475f6dfdc  boot.img
527ef7a81e366f4403ce6d3afc408c50f4bc8c8731a55a912b9a70d8167b3338  rootfs.img
513d2dde74103b047303f1cef1e81634a322befe25fc981c0008e54ff45a683d  oem.img
```

## 由用户烧录

开发板进入 Rockchip USB Loader/MaskRom 后，在本机执行：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/flash-aura-emmc
```

这个脚本全量写入分区，也会覆盖 `/userdata` 中已部署的 sender 和采集文件。
本次生成的 `update.img` 打包日志没有包含 growable rootfs；请使用上述分区脚本，
保证写入最新 `rootfs.img`，不要只刷 `update.img`。

开机接网线后先验证枚举和芯片 ID：

```sh
ssh root@192.168.1.10 'sh -s -- --probe' < tools/diagnose-ar0234
```

密码为 `luckfox`。ID 和媒体链路确认正常后，再按
[SSH 诊断流程](ar0234-ssh-diagnostics.md) 采集 120 帧。

## 首次实机测试后的修复镜像（当前最新）

2026-10-12 14:16–14:17，在相同 AR0234 BoardConfig 下再次执行
`./build.sh all`，退出码 0。kernel 更新到 `6f1aee66d`，修复 CIF 将
1920×1200 像素阵列边界误作 1280×720 输入帧裁剪边界的问题。
诊断脚本同时支持 sensor/CIF 与 ISP 分属两个 media graph 的拓扑。
首次实机采集仍失败，具体证据见 [实机测试记录](ar0234-hardware-test.md)。

22 项自动化测试、驱动 `.o/.ko` 的 `W=1 + -Werror` 编译链接通过。
从最新 `boot.img` 提取的内核与构建产物一致，DTB 四项测试通过；
最新 `oem.img` 中五个 MPP/Rockit 模块 vermagic 与 PREEMPT 内核匹配。
sender runtime 检查、烧录脚本 dry-run 通过，没有自动烧录或重启。

当前 SHA-256：

```text
283720ea3c4cbf72a9ead3f926c8e6b0a7381470128ae89d3c05536f3ec352db  boot.img
6440821de4f09c5e633e0c238347a794e410623a11dbf523301cb2ed0eb66bde  rootfs.img
87894b73299d60e7af6119b453cb83e777bd41940266b8966a47eaecffab6d90  oem.img
```

烧录命令仍为 `./tools/flash-aura-emmc`，覆盖 `/userdata`；烧录后需要
重新实机采集验证修复效果，尚不能宣称成功出帧或达到 99.23 fps。
新构建日志：`logs/ar0234-hardware/crop-fix-full-build.log`。
