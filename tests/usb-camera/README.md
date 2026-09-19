# USB 摄像头本机闭环测试

当前 `/dev/video0` 是 UVC MJPEG 摄像头，支持 1280×720@60。以下命令在 CachyOS 宿主机完成一条可视的本机验证链路：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/test-usb-camera-loopback
```

数据通路为：

```text
/dev/video0 MJPEG → FFmpeg MJPEG decode → libx264 zerolatency
→ 自定义 UDP → FFmpeg VAAPI H.264 decode → DRM PRIME → Vulkan/SDL3
```

验收条件：sender 和 viewer 都输出 120 个 access unit，viewer 输出 `hardware_frames=120` 和 `presenter: frames=120`，且 `malformed=0`、`dropped=0`。

这是 Aura 硬件到达前的端到端协议与显示验证。USB sender 的 CPU MJPEG 解码和 libx264 编码不用于最终开发板性能数据；Aura sender 将替换为 VI/ISP → RK MPP VENC。
