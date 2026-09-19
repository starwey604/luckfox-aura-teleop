# H.264 VAAPI 实机测试

此测试必须从 CachyOS 宿主机执行，不能在 Distrobox 内运行：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/test-vaapi-decoder
```

脚本会用 `libx264` 生成 120 帧、640×360@60 fps 的 Annex-B H.264 流，再调用 `host-viewer --input`。验收输出必须包含：

```text
decoder: frames=120 hardware_frames=120
```

`hardware_frames` 必须等于 `frames`；否则该测试不能证明应用使用了 VAAPI 硬件 surface。脚本要求 CachyOS 已安装 `ffmpeg` 和 `ffmpeg` 的开发包，且当前用户可访问 `/dev/dri/renderD128`。

完整显示路径额外执行：

```sh
./tools/test-vulkan-presenter
```

它还要求输出 `presenter: frames=120`，证明 VAAPI frame 已由 libplacebo 转为 DRM PRIME 并导入 Vulkan，在 SDL3 创建的窗口中呈现。
