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

[内核/Buildroot 裁剪与编译优化计划](docs/kernel-buildroot-optimization-plan.md)
及 [双路延迟基线与 A/B 测试](docs/stereo-latency-baseline-plan.md)：先冻结并测量现有镜像，
再逐项裁剪、调优，保留可以复测的结果。

[软件性能基线首轮](docs/software-performance-baseline-2026-10-12.md)：`TELEOP_METRICS=1`
启用板端/主机局部阶段统计。KMPP IRQ修复已通过
[短测生命周期回归](docs/kmpp-meta-irqfix-runtime-2026-10-13.md)。

[完整帧立即发送/解码优化](docs/frame-boundary-optimization-2026-10-13.md)：sender默认mode1，
完整AU标记让viewer立即提交VAAPI；双路短测中移除了约17.7ms的两项软件等待。
最终屏幕延迟与长时间稳定性仍待验收；程序更新不需要烧写镜像。

[Viewer 解耦与队列观测](docs/viewer-pipeline-decoupling-2026-10-13.md)：默认独立收包、
每路解码和主线程显示，提供同步对照与运行中队列统计；双路短测已验证显示阻塞隔离。

[重启后实机数据与温度核对](docs/software-performance-baseline-reboot-2026-10-12.md)：
`tools/test-aura-software-baseline` 自动配对 SSH sender 与本机 headless viewer，默认短测并检查启动温度。
