# 同事如何复现当前双 AR0234 镜像

2026-10-12 核对：驱动、DTS、内核 fragment、Buildroot defconfig、BoardConfig、
静态网络/performance overlay 均已保存为 `sdk/patches/` 下的补丁。
sender、viewer、诊断和双摄启动脚本直接保存在本仓库源码中。
相机运行时 exposure/gain 由 `tools/run-ar0234-stereo` 每次启动应用。
不依赖当前开发板上的手工设置。

## 版本条件

支持的厂商基线是 `luckfox_aura_v1.0.1_20260518`，内核 6.1.141、
Buildroot 2025.02.6。`sdk/manifest.lock.xml` 锁定全部 63 个厂商子仓库，
`sdk/series.tsv` 定义 4 个补丁目标及其基线。

“旧 SDK”需要区分：

- 同一厂商基线、尚未应用 teleop 补丁：可以直接应用。
- 同一基线、已应用本仓库较早的完整补丁前缀：可以补齐缺少的补丁。
- 不同厂商版本，或者夹杂同事自己的改动：不能保证直接应用。
  保存自己的修改，另建匹配版本的 SDK；需要保留其它版本时，应单独移植并回归。

本仓库没有厂商 SDK 本体、工具链二进制或下载缓存；拉取 teleop 不能代替
准备 SDK。锁定 manifest 的 remote fetch 是厂商 SDK 的相对路径 `.`，它用于
记录版本，不能凭这一个 XML 当作公开下载 URL 直接 `repo init`。
应先取得该版厂商 SDK/其 repo 镜像来源，再核对本仓库的锁定提交。
不要在有个人修改的 SDK 上自动 reset、clean 或强制 sync。

先确保本仓库最新提交已推到远端。核对时本地 `main` 比 GitHub 的 main 多 7 个
提交，其中最新双摄提交为 `d88d1e5`；本次复现工具修复也必须推送。
同事只 pull 当时的远端 main，拿不到这些未推送提交。

## 构建步骤

以下命令在 **teleop 仓库根目录** 运行。把 SDK 路径替换成同事的真实绝对路径；
无需使用 `/home/ww`，也不要求两个仓库紧挨着。

```sh
SDK=/absolute/path/to/aura-sdk
export AURA_SDK_ROOT="$SDK"

# 全部厂商子仓库版本/已保存补丁前缀检查，不切换 SDK 的工作树。
./tools/verify-sdk-baseline "$SDK"
./tools/sdk-patch apply "$SDK"
./tools/select-teleop-board "$SDK" ar0234-stereo

# SDK 与目标程序只能在已准备好的 Ubuntu 22.04 Distrobox 中构建。
distrobox enter aura-sdk -- env AURA_SDK_ROOT="$SDK" TELEOP_ROOT="$PWD" \
  bash -lc 'cd "$AURA_SDK_ROOT" && ./build.sh all &&
            cd "$TELEOP_ROOT" && ./tools/build-aura && ./tools/check-aura-runtime'
```

需要预先安装 SDK 要求的构建依赖；尤其要有 automake/aclocal-1.16、autoconf、
m4 等。版本检查不检测已安装软件包，也不自动下载或安装它们。
`./build.sh all` 包含 media 构建，为交叉编译 sender 提供头文件和库。

使用干净 SDK 构建目录，避免旧版内核、Buildroot 配置和媒体模块缓存混用。
本脚本不擅自删除旧产物；已有大量不同配置的构建缓存时，推荐新目录复现。
MPP/Rockit 模块必须与启用 PREEMPT 的当前内核一起重新构建，不能拷贝旧 `.ko`。

如果基线检查拒绝：按输出的仓库路径检查 HEAD 与所需提交。历史太浅或对象
缺失时，需要从对应厂商来源补齐历史/重新取得匹配 SDK；脚本不会强行移植。

可选：在 Ubuntu Distrobox 中运行本次采用的源码复现测试。
它从现有 SDK 的 Git 对象建立临时干净基线副本，实际应用补丁并逐仓库比较
Git tree；不修改当前 SDK 的 HEAD、index 或工作树，不执行完整镜像构建。

```sh
distrobox enter aura-sdk -- env AURA_SDK_ROOT="$SDK" TELEOP_ROOT="$PWD" \
  bash -lc 'cd "$TELEOP_ROOT" && python3 tests/sdk/test_patch_stack.py &&
            ./tools/test-sdk-replay "$AURA_SDK_ROOT"'
```

## 镜像与应用分开交付

`./build.sh all` 输出 `SDK/output/image/`；**sender 和双摄脚本不会自动打包进
这些镜像**。它们按目前开发方案单独交叉编译、SSH 部署到 `/userdata`：

```sh
# 先检查完整分区烧录计划，再由操作者执行烧录。
AURA_SDK_ROOT="$SDK" ./tools/flash-aura-emmc --dry-run
# AURA_SDK_ROOT="$SDK" ./tools/flash-aura-emmc

# 烧录并启动后，SSH root@192.168.1.10，密码 luckfox。
./tools/deploy-aura
```

完整烧录会覆盖 `/userdata`，因此要在烧录后部署程序。
使用项目全量分区脚本，不要仅烧 `update.img`：当前 `update.img` 未包含 growable
rootfs，厂商 `rkdownload.sh` 也会因分区长度 `-` 跳过 rootfs。

本机 viewer 在 CachyOS 原生构建，**不能在 Distrobox 编译**：

```sh
./tools/build-host
./build/host/bin/host-viewer --port 5000 --dual-stream --frames 0 --timeout-ms 3600000
ssh -t root@192.168.1.10 /userdata/run-ar0234-stereo
```

主机直连网口需要 `192.168.1.2/24`。双摄脚本自动配置亮度，默认关闭双路 wrap，
先等 camera0 就绪再启动 camera1；两路仍并发回传。直接手工启动两个 sender
而不指定这些参数，不等价于已验证的双摄运行方式。

## 本次验证与“相同镜像”的范围

全部 63 个现用子仓库：未打补丁的 HEAD 与锁定 manifest 一致，跟踪文件没有
遗漏的未提交修改。4 个目标仓库的干净基线补丁重放结果与当前 SDK 源码 tree 相同：

```text
sysdrv/source/kernel           3cfbce5901e270715255cd02c9cdc9c9778421a7
sysdrv                         e5c3cae762a88bbfe5cb33ba087af8feadfc9a87
project                        58cb7964f1631afd2b2a775b5a83b3e87afcde86
project/cfg/BoardConfig_Aura     0a6999a32b2e89451dc761c6a227281158759150
```

Ubuntu Distrobox 中，SDK 工具 8 项回归与实际补丁重放/重复应用通过。
之前已完成当前源码的整套镜像构建和双摄短时实机测试，见
[双 AR0234 构建及实机记录](ar0234-stereo-bringup.md)。本次没有另做一次全量构建。

这里保证的是可恢复同一套**源码和配置**，并提供一致的构建/部署步骤；不能仅靠
pull 保证镜像 SHA-256 相同。内核构建时间、构建用户名/主机名、git am 生成的
提交时间、ext4 UUID/时间戳等未全部固定，目前还不是逐字节可复现构建。
同事环境仍应验证打包的 DTB、PREEMPT 模块匹配、SSH、两路采集和回传。

## 生命周期诊断 profile（2026-10-12）

可选 `./tools/select-teleop-board ../aura-sdk ar0234-stereo-debug`，随后用 Distrobox
运行 `./build.sh driver`（内核+外部模块）和 `./build.sh firmware`。其余 profile 不变。
诊断版增加 stacktrace/hung-task/netconsole，关闭 lockup/stall 默认 panic；不作为延迟 baseline。
详情见 [本轮排查记录](sender-lifecycle-debug-2026-10-12.md)。
