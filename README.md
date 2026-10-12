# Luckfox Aura Teleop

RV1126B 双摄低延迟回传：板端 C++ RK MPI/MPP H.264 sender，Linux 本机
FFmpeg VAAPI 解码与 SDL3/Vulkan 显示。厂商 SDK 由独立 repo 工作树管理，
本仓库保存 SDK 补丁、应用源码、构建/部署工具和验证记录。

同事首次构建或从旧 SDK 迁移，先读
[SDK 复现步骤](docs/sdk-reproduction.md)。当前厂商基线为
`luckfox_aura_v1.0.1_20260518`，双 AR0234 选择 `ar0234-stereo`。
SDK/板端程序在 Ubuntu 22.04 Distrobox 编译，本机 viewer 在 CachyOS 原生编译。

[双 AR0234 实机结果与启动方式](docs/ar0234-stereo-bringup.md)：两路 1280×720
约 99.23 fps，目前双路默认关闭 wrap 并错开初始化；长时间稳定性与新的
screen-to-screen 延迟仍待验收。
