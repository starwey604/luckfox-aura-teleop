# Luckfox Aura Teleop 工作区与 SDK 开发方案

## 1. 目标

本项目在不直接维护整套厂商 SDK fork 的前提下，为 Luckfox Aura（RV1126B）建立可重复、可审查、可升级的开发工作区。`luckfox-aura-teleop` 是配置、补丁、应用源码、构建说明和测试结果的唯一来源；`aura-sdk` 是可以重新同步和重新应用补丁的厂商工作树。

所有构建在 Ubuntu 22.04 Distrobox 容器中执行。宿主机与容器共享 `/home/ww/codings`，因此项目仓库和 SDK 不需要在容器里重复克隆。

## 2. 为什么使用跨子仓库 patch stack

Aura SDK 由 Android `repo` 管理，不是单一 Git 仓库。当前修改可能分布在：

| SDK 路径 | 所属内容 |
|---|---|
| `sysdrv/source/kernel` | Linux、Kconfig、DTS、摄像头驱动 |
| `sysdrv` | Buildroot 压缩包、defconfig、系统构建 Makefile |
| `project/cfg/BoardConfig_Aura` | Aura BoardConfig 和 rootfs overlay |
| `project` | 总构建脚本和通用应用构建逻辑 |
| `media` | RK MPI/MPP、ISP 和媒体组件集成 |
| `media/mpp/mpp` | Rockchip MPP 独立子仓库 |

在 SDK 根目录运行一次普通 `git diff` 无法覆盖这些嵌套仓库。为整套 SDK 建一个巨型 fork 也会丢失 manifest 对各项目版本的描述，使厂商升级难以合并。因此采用以下模型：

1. 保存厂商 manifest 和每个相关项目的基线 commit。
2. 每个被修改的 SDK 子仓库使用独立 `teleop/*` 分支。
3. 使用 `git format-patch` 导出有提交说明的补丁。
4. 由本项目中的统一脚本按顺序检查并应用补丁。
5. SDK 工作树可以随时由同一 manifest 重新创建。

## 3. 推荐仓库结构

```text
luckfox-aura-teleop/
├── docs/
│   ├── workspace-sdk-development-plan.md
│   └── camera-low-latency-tuning-plan.md
├── sdk/
│   ├── manifest.lock.xml
│   ├── baseline.toml
│   ├── series.toml
│   └── patches/
│       ├── sysdrv/source/kernel/
│       ├── sysdrv/
│       ├── project/
│       ├── project/cfg/BoardConfig_Aura/
│       └── media/
├── configs/
│   ├── kernel/
│   ├── buildroot/
│   └── board/
├── rootfs-overlay/
│   └── etc/init.d/
├── src/
│   └── teleop-sender/
├── tests/
│   └── latency/
└── tools/
    └── sdk-patch
```

`sdk/patches` 中的目录名与 `repo` manifest 的项目路径保持一致，补丁应用目标因此不需要猜测。应用源码放在 `src`，不要长期把唯一副本留在 SDK 的 sample 目录中。

## 4. 当前 SDK 基线

当前 checkout 使用：

```text
manifest: luckfox_aura_v1.0.1_20260518.xml
kernel:   b8187ea6c6dbd97deee68cfe121bedc66219262e
sysdrv:   409e2453c14560081f3e7299b6b13b364a8f086a
board:    b5a3608941ca6f64ee670e8d2a2a652d26734724
media:    fb94466e2a3227eada0e9ce1b02db58ec4b3b33e
project:  5bbb44615deacd038f659baa8f9d06f3eeb6162e
```

首次建立补丁目录时保存完整 pinned manifest：

```sh
cd /home/ww/codings/aura-sdk

.repo/repo/repo manifest -r \
  -o /home/ww/codings/luckfox-aura-teleop/sdk/manifest.lock.xml
```

同时在 `baseline.toml` 中记录 SDK 发布名称、相关项目 commit、构建容器镜像和工具链版本。manifest 是机器可恢复的完整基线，`baseline.toml` 是便于人工检查的摘要。

## 5. 子仓库开发流程

### 5.1 建立功能分支

每个修改过的项目单独建分支：

```sh
git -C /home/ww/codings/aura-sdk/sysdrv/source/kernel \
  switch -c teleop/kernel-low-latency

git -C /home/ww/codings/aura-sdk/project/cfg/BoardConfig_Aura \
  switch -c teleop/aura-board

git -C /home/ww/codings/aura-sdk/sysdrv \
  switch -c teleop/buildroot
```

提交应按可独立审查和回退的功能拆分，例如：

```text
kernel: add teleop low-latency config fragment
arm64: dts: add Aura dual MIA1321 teleop configuration
buildroot: add luckfox_64bit_teleop_defconfig
board: add teleop BoardConfig and rootfs overlay
media: add low-latency VI-to-VENC UDP sender
```

不要在一个提交中同时修改摄像头 DTS、内核抢占、Buildroot 包和编码程序。

### 5.2 导出补丁

以 kernel 为例：

```sh
git -C /home/ww/codings/aura-sdk/sysdrv/source/kernel \
  format-patch b8187ea6c6dbd97deee68cfe121bedc66219262e..HEAD \
  -o /home/ww/codings/luckfox-aura-teleop/sdk/patches/sysdrv/source/kernel
```

其他项目使用 `baseline.toml` 中对应的基线 commit。生成后检查补丁标题、作者、二进制文件和无关格式变化。

### 5.3 应用补丁

```sh
git -C "$SDK/sysdrv/source/kernel" am --3way \
  "$TELEOP/sdk/patches/sysdrv/source/kernel/"*.patch
```

精确基线上的补丁应直接应用；`--3way` 只用于小范围上下文变化。SDK 当前很多项目是 shallow clone，升级后若需要三方合并，应先对受影响的项目补充相应历史。

## 6. `tools/sdk-patch` 的职责

补丁管理脚本建议提供以下命令：

```text
sdk-patch check  <sdk-path>
sdk-patch apply  <sdk-path>
sdk-patch status <sdk-path>
sdk-patch export <sdk-path>
```

`check` 必须：

- 确认目标目录包含 `.repo`；
- 比对 manifest 名称和相关项目 commit；
- 检查每个目标子仓库是否干净；
- 对全部 patch 执行预检查；
- 发现错误时不修改任何仓库。

`apply` 必须：

- 按 `series.toml` 的确定顺序运行；
- 每个项目使用 `git am --3way`；
- 任一项目失败时停止并显示恢复命令；
- 已经应用的 patch 不重复应用；
- 最后输出所有项目的新 HEAD 和工作树状态。

`status` 只读地报告基线、当前 HEAD、ahead commit 和未提交修改。`export` 只从明确列出的 `teleop/*` 分支导出补丁，避免把 SDK 中的临时实验全部收进项目。

脚本不应自动执行 `git reset --hard`、`git clean` 或删除构建产物。

## 7. 配置如何落入 SDK

### 7.1 BoardConfig

新增独立文件：

```text
BoardConfig-EMMC-RK801-Luckfox_Aura-Teleop-Buildroot.mk
```

它从厂商 Aura Buildroot 配置派生，但显式选择：

- teleop kernel fragment；
- teleop DTS；
- `luckfox_64bit_teleop_defconfig`；
- teleop rootfs overlay；
- `RK_BOOTARGS_CMA_SIZE="256M"`；
- 精简后的应用集合。

原厂 BoardConfig 保持可用，以便生成 factory baseline 镜像。

### 7.2 内核配置

SDK 原生支持 `RK_KERNEL_DEFCONFIG_FRAGMENT`。将低延迟配置作为独立 fragment 添加到 kernel 仓库，并从 teleop BoardConfig 引用。功能稳定后再生成完整 minimal defconfig；开发阶段优先使用 fragment，让差异容易审查。

DTS、DTSI 和 MIA1321 驱动也属于 kernel 子仓库，使用 kernel patch 管理。

### 7.3 Buildroot

Buildroot 2025.02.6 会从 `sysdrv/tools/board/buildroot/buildroot-2025.02.6.tar.gz` 解压。SDK 当前 Makefile 只把两个厂商 defconfig 复制进解压目录。因此应：

1. 在 `sysdrv/tools/board/buildroot` 新增 `luckfox_64bit_teleop_defconfig`；
2. 修改 `sysdrv/Makefile`，使 `buildroot_create` 同时复制 teleop defconfig；
3. BoardConfig 设置 `RK_BUILDROOT_DEFCONFIG=luckfox_64bit_teleop_defconfig`。

不要只修改解压后的 `sysdrv/source/buildroot/buildroot-2025.02.6`，该目录会被 `buildroot_create` 删除重建。

后续自有应用稳定后，可以增加很小的 `BR2_EXTERNAL` 接入 patch，让 `src/teleop-sender` 由 Buildroot 正式打包；初期可以先由 media 工具链编译，再通过 overlay 安装。

### 7.4 Rootfs overlay

BoardConfig 已支持 `RK_POST_OVERLAY`。新增 `overlay-luckfox-teleop`，包含：

- CPU governor 启动脚本；
- teleop 网络配置；
- 摄像头发送程序启动脚本；
- 必要配置文件。

开发镜像保留 SSH 和诊断工具；最终压测镜像可以用另一份 overlay 删除这些服务。

当前厂商 `build.sh` 的 `check_config` 不能正确检查含空格的 `RK_POST_OVERLAY` 值。
Teleop BoardConfig 因此保留厂商的单一 overlay，并用 `RK_POST_BUILD_SCRIPT` 安装
`overlay-luckfox-teleop`。该 overlay 令 RJ45 的 `eth0` 固定为 `192.168.1.10/24`，
禁用 DHCP、IPv4LL 和默认路由；CachyOS 直连端使用 `192.168.1.2/24`。

## 8. Fork 策略

应把 `luckfox-aura-teleop` 推送到自己的 GitHub、GitLab 或其他远端，并保护主分支。厂商整套 SDK 不需要 fork。

以下情况适合单独 fork 某个上游子仓库：

- MIA1321/ISG1321 驱动形成长期维护的较大改动；
- 需要持续合并 Rockchip kernel 或 MPP 更新；
- 准备向厂商提交 pull request；
- 单个项目的补丁数量已经难以作为短 patch series 审查。

自己的 UDP sender、测试工具和接收端程序始终直接保存在 `luckfox-aura-teleop`，不需要先进入厂商仓库再导出。

## 9. 构建与镜像序列

每一轮构建使用同一容器和明确的 BoardConfig：

```sh
distrobox enter aura-sdk
cd /home/ww/codings/aura-sdk

./build.sh lunch
./build.sh kernelconfig
./build.sh kernel
./build.sh buildrootconfig
./build.sh rootfs
./build.sh firmware
```

该板卡的 rootfs 分区在当前 BoardConfig 中使用剩余 eMMC 空间。SDK 因而会生成
`output/image/rootfs.img`，但 `update.img` 的打包列表不包含 rootfs。使用 USB eMMC
烧录工具时，应写入新的 `rootfs.img` 到 rootfs 分区；不能只刷 `update.img` 来更新
rootfs 中的静态网络配置。

### USB eMMC 烧录注意事项

当前 SDK 附带的 `tools/linux/Linux_Upgrade_Tool/rkdownload.sh` 会把末尾的
`-(rootfs)` 分区大小原样传给 `upgrade_tool wl`。该工具不接受 `-` 作为扇区数，因而
`rkdownload.sh -d output/image` 会在前序分区成功后，以 `Size is invalid` 跳过
`rootfs.img`。不要把该命令的退出过程视为完整烧录成功。

板子已进入 Loader 模式时，直接省略 `wl` 的可选扇区数量参数，让工具根据镜像文件
长度写入 rootfs：

```sh
cd /home/ww/codings/aura-sdk/tools/linux/Linux_Upgrade_Tool
sudo ./upgrade_tool wl 0x607C40 ../../../output/image/rootfs.img
sudo ./upgrade_tool rd
```

`0x607C40` 来自当前 teleop `env.img` 的 rootfs 起始 LBA；不要对不同 BoardConfig
或修改过分区表的镜像复用该值。若设备处于 MaskRom 而非 Loader 模式，先运行
`sudo ./upgrade_tool db ../../../output/image/download.bin`，再执行上述 `wl`。

首次完整烧录可先用 `rkdownload.sh -d` 写入固定大小的分区，再立即运行上述两条命令
补写 rootfs。日常仅更新根文件系统时，也只执行这两条命令即可。

### USB CDC ACM 调试串口

teleop BoardConfig 将 Aura 的 `USB3 OTG` Type-C 口配置为 USB gadget CDC ACM：板端是
`/dev/ttyGS0`，CachyOS 主机枚举为 `/dev/ttyACM*`。内核日志会在 gadget 控制器可用后
输出到该接口，Buildroot 也会在 `ttyGS0` 上运行 root 登录终端。物理 UART 控制台保持
不变，作为无法启动时的恢复手段；USB 串口不能提供 U-Boot 或最早期内核初始化日志。

将板子用独立的 5V/3A 电源启动，并以数据线连接主机与 `USB3 OTG` 后，在主机检查：

```sh
sudo dmesg -w
ls -l /dev/ttyACM*
```

需要交互式登录时使用 `picocom -b 115200 /dev/ttyACM0`。如果主机未安装 picocom，先由
用户安装 `picocom` 包；只看日志可使用 `sudo cat /dev/ttyACM0`。烧录模式与正常 gadget
模式共用该 OTG 口，因此进入 Rockchip Loader 后不会枚举为 ACM 串口。

推荐镜像序列：

1. `factory-baseline`：未经裁切的厂商配置。
2. `teleop-tools`：只加入 GStreamer、v4l2-utils、ethtool 和 tcpdump。
3. `teleop-preempt`：加入 PREEMPT、HZ=1000、CPU performance。
4. `teleop-camera1`：单路摄像头和 UDP。
5. `teleop-camera2`：双摄 DTS 和双路编码。
6. `teleop-minimal`：裁掉音频、显示、无线、USB 和无关服务。
7. `teleop-perf`：单独测试 kernel `-O2`、wrap、slice、IRQ affinity 和编码参数。

只有当前镜像通过回归后才开始下一轮，不在同一个镜像中同时改变所有变量。

## 10. 构建产物与实验记录

每次有效构建至少保存：

```text
artifacts/<date>-<image-name>/
├── manifest.xml
├── baseline.toml
├── board-config.mk
├── kernel.config
├── buildroot.config
├── image.sha256
├── boot.log
└── test-report.md
```

镜像本体通常较大，可以存放在 release、对象存储或本地归档目录；Git 中至少保存 SHA-256、生成命令和对应配置。测试报告记录启动、网络、摄像头、编码、温度及延迟结果。

## 11. SDK 升级流程

升级厂商 SDK 时：

1. 在新目录同步新 manifest，不覆盖现有可工作的 SDK。
2. 运行 `sdk-patch check`，得到基线差异和预计冲突。
3. 按 kernel、sysdrv、BoardConfig、project、media 的顺序应用 patch。
4. 在对应子仓库解决冲突并重新导出 patch。
5. 先构建 `factory-baseline`，再构建 teleop 镜像。
6. 完整执行网络、单摄、双摄、编码和延迟回归。
7. 更新 `manifest.lock.xml` 和 `baseline.toml`。

不要在确认新版本通过回归前删除旧 SDK 和旧镜像。

## 12. 近期实施顺序

第 0、1 轮已经完成前五项。`sdk-patch` 已在临时的精确基线 worktree 上通过 `check → apply → status` 验证。

Teleop 的 BoardConfig 选择不通过交互式 `lunch` 完成，而使用项目脚本：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/select-teleop-board /home/ww/codings/aura-sdk
```

当前 Teleop Buildroot 使用 GCC 12 而非厂商 defconfig 的 GCC 13。原因是该 SDK 附带的 Buildroot 2025.02.6 没有让 GCC 13 选择内部的 `GCC >= 12` Kconfig 能力，因而会静默移除 `v4l-utils`。GCC 12 可正常保留 `v4l2-ctl` 和 `media-ctl`；原厂 GCC 13 defconfig 没有修改。

下一步是让首次 Buildroot 完整构建完成，确认 rootfs 内容；实物到达后按摄像头延迟调优文档逐阶段产生 patch 和镜像。
