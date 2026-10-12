# 未裁剪内核：软件性能基线首轮

本轮只测软件阶段、帧率和稳定性；screen-to-screen 留作最终结果验证。
**板端原始采集通过，双路编码回传测试中板端失联；尚未取得完整的实机软件延迟基线。**
为继续推进，补充了可关闭的计时功能，并完成本机真实 720p 码流回放测试。
本机回放结果与实机链路分开记录，不用回放数字冒充板端/网络端到端延迟。

## 1. 本轮实机核对与异常

- 板端 `Linux 6.1.141 #12 SMP PREEMPT Mon Oct 12 14:16:35 CST 2026`，双 AR0234 DTS。
- `/userdata/teleop-sender` 的 SHA-256 为
  `306db20a9a7cb96a13af071f9015600b388f0a1ff077291cd72f6ae5e308536c`，与归档旧程序一致。
- 启动参数仍为 `rk_dma_heap_cma=256M`；没有改内核、频率、IRQ、NIC 或 socket 上限。
- Wi-Fi、Bluetooth、nginx/fcgiwrap、ADB、NTP、RKIPC 启动外壳等后台仍在，属于未裁剪系统。
- CPUFreq 配置虽启用，运行时没有 `policy*` 节点。Aura 公共 DTS 对 `cpu0` 删除了 `operating-points-v2`，
  与运行时观察一致；不能宣称 performance 启动脚本已经锁住 CPU。实际 CPU 时钟尚待读取。
- devfreq 只发现 VENC 与 NPU：VENC `venc_ondemand`，当时 550 MHz，可用 396/550 MHz；
  NPU `rknpu_ondemand`，当时 800 MHz。没有发现 DMC devfreq 节点；基础 DTS 中 DMC 是 disabled。
  后续调频需要先确认 DTS/OPP/驱动，不能只增加 performance governor 就假定 DDR 已锁频。
- 板端 EEE disabled；STMMAC coalescing 为 RX 327 us、TX 1000 us。没有修改。
- 板端 `rmem_max/wmem_max` 均为 212992；旧程序请求 4 MiB，不等于实际获得 4 MiB。
- 空闲快照温度 36.26°C；失联后未获得运行中温度、CPU 占用或驱动状态。

先运行旧 launcher 的 `--capture-only --frames 600`，成功退出。随后用归档旧 viewer 的
`--dual-stream --headless --frames 12000 --timeout-ms 15000` 和旧 sender 每路 6000 帧尝试编码测试。
viewer 出现网络 inactivity timeout；sender 日志显示两路进程完成初始化后，SSH 超时。
之后 ping 无响应、ARP FAILED、SSH 超时/No route to host。
该次测试没有可用的最终编码/解码计数；不能据此判断在哪一个具体阶段卡住，
也不能把 viewer 的超时当作单向网络延迟。

**失联发生在部署新计时程序之前。** 本轮没有向板子部署计时版程序，旧 sender 仍保留；
没有自动烧录或重启，已经请求用户手动重启。
下一次恢复后先取 `dmesg/pstore`、核对 receiver 端口持续可用，再由单路短测试逐步增加到双路，
同步保存可在本机保留下来的 SSH 监控日志。

## 2. 原始采集结果

两路各返回 600 个 NV12 720p 帧，每帧 1382400 字节。
序号均从 0 到 620，启动段各跳过 21 帧；不把启动现象藏在稳态统计里。
稳态取第 41 个返回帧以后，559 个间隔样本/路，序号连续无缺帧。

| 摄像头 | 稳态 fps | 平均间隔 ms | P50 ms | P95 ms | P99 ms | 最大 ms |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 99.23234 | 10.07736 | 10.077 | 10.102 | 10.121 | 10.122 |
| 1 | 99.23187 | 10.07741 | 10.077 | 10.116 | 10.121 | 10.122 |

这是 V4L2 采集时间戳的间隔，不是曝光到编码输出延迟，也不是跨网传输延迟。
六秒采集通过不能证明编码路径或长时稳定性通过。

## 3. 新增分段计时的范围

sender 与 viewer 都用 `TELEOP_METRICS=1` 显式打开；默认关闭。
保留 wire protocol、PTS 分组、VI/VENC buffer 数量、编码设置和解码策略。
分开构建到 `build/aura-metrics`、`build/host-metrics`，现有归档和原来的 build 目录没有被替换。

所有软件阶段使用各自进程的 `CLOCK_MONOTONIC`，不相减板端和主机时钟。
直方图固定内存、计入全部样本；百分位为 bin 上界，bin 相对宽度不超过约 1.563%，
min/mean/max 是实际观测值。默认关闭时不采样时钟、不记录直方图。
计时模式会增加少量开销，后续比较必须两组都开启相同模式。

| 指标 | 测量定义 | 不包含/不能说明 |
|---|---|---|
| `sender.get_stream_wait` | RK MPI GetStream 调用耗时 | 主要可能是等数据，不等于 VENC 硬件编码执行时间 |
| `sender.au_pack_span` | 首个 pack 返回到最后一个同 PTS pack 返回 | 非传感器曝光开始，也不等于物理 slice 执行边界 |
| `sender.next_pts_wait` | 上一 AU 最后 pack 到下一 PTS 确认该 AU 完整 | 包含调度/应用处理，待实机测出具体值 |
| `sender.first_pack_to_send` | 首 pack 到开始处理完整 AU 发送 | 解释完整帧聚合等待 |
| `sender.udp_queue` | packetize/sendmmsg 到排队完成 | 不代表已经发到线缆或到达接收机 |
| `host.streamN.reassembly` | 首个被重组器接受的数据报到完整 AU 就绪 | 不包括在 socket 中先前积压的时间，不是单向网络延迟 |
| `host.streamN.au_interval` | 两个完整 AU 就绪的间隔 | 可观察吞吐/抖动 |
| `host.streamN.parser_wait` | AU 就绪到 parser 将它提交给解码器 | 直接测 parser 的边界/lookahead 等待 |
| `host.streamN.submit_to_frame` | packet 提交到 FFmpeg 返回对应 VAAPI frame | 是 API/帧可用耗时；未同步 GPU fence，不是完整 GPU 执行时间 |
| `host.streamN.au_ready_to_frame` | 完整 AU 就绪到 FFmpeg 返回对应 frame | 主机局部路径，包含 parser 等待，不包含面板显示 |
| `host.streamN.parser_decode_call` | 单次 parser 调用及其同步 decode/sink 回调耗时 | 可能处理前一 AU；有显示时包含 present 调用 |
| `host.streamN.present_call` | 对应 frame 得到后到显示函数返回 | 不代表面板发光；本轮 headless 不测它 |

通过 FFmpeg parser/packet/frame 的 PTS 把**主机局部 AU 就绪时间**关联到相应解码 frame，
不使用来自板子的 capture timestamp 做跨机计算。序号乱序、两路区分和末帧 flush 的关联已在真实 VAAPI 测试覆盖。
诊断模式还打印 socket 请求值与 `getsockopt` 的 Linux 实际值；Linux 记账倍数应与请求值区别阅读。
当前函数只从单个应用线程调用；未来拆线程时需同时调整统计同步设计。

## 4. 本机真实 720p 回放结果（不是实机网络结果）

使用此前保存的两个正确 AR0234 H.264 720p 文件，各 300 帧，循环到每路 3000 帧。
按约 99.23 fps 的绝对时间间隔，通过 localhost UDP 交给计时版 viewer，默认 VAAPI，headless。
没有 software-decode、显示窗口、重新编码或板端参与。

约 30.23 秒结束：

```text
access_units=6000 datagrams=119710 bytes=140210480
malformed=0 duplicates=0 dropped=0
stream0: frames=3000 hardware_frames=3000
stream1: frames=3000 hardware_frames=3000
```

每个下列 AU 指标均有 3000 样本；包含初始化与末帧 flush，没有按结果挑除异常。
表中百分位是直方图上界。

| 指标 | stream0 P50/P95/P99 ms | stream1 P50/P95/P99 ms |
|---|---|---|
| 首数据报到 AU 重组完成 | 0.0599 / 0.1475 / 0.2273 | 0.0084 / 0.0269 / 0.0420 |
| parser 等待 | 10.0925 / 10.2236 / 10.3547 | 10.0925 / 10.3547 / 10.4858 |
| packet 提交到 VAAPI frame 返回 | 0.1157 / 0.2150 / 0.2744 | 0.0476 / 0.0891 / 0.1270 |
| 完整 AU 就绪到 frame 返回 | 10.2236 / 10.4858 / 10.6168 | 10.2236 / 10.3547 / 10.4858 |

两份码流内容/大小不同，注入顺序也不同，不能把两路微小差异当作摄像头硬件快慢。
VAAPI frame 返回可能仍是异步 GPU 工作的句柄，这些很小的 API 数字不能写成“硬件完整解码只需 0.1 ms”。
没有测量跨主机单向 UDP 时间、板端编码时间、采集到主机的总延迟或出屏耗时。

可确定的发现是：**当前 parser 在这个完整 AU 回放场景中等待了约一个帧周期**。
下一轮优先试验明确“完整 AU”的网络入口，绕过不必要的 parser lookahead；
仅用于已经可靠重组的 AU，不改变文件/旧 slice bytestream 的解析，也不直接把不完整 slice 送给硬解。
这是后续实验方向，本轮没有改变默认解析行为；收益还须真实链路验证。

## 5. 验证和继续测试命令

本轮通过：

- CachyOS 原生构建 viewer；Distrobox Ubuntu 22.04 交叉构建 sender，ELF 为 AArch64，`/oem/usr/lib` RPATH。
- native 和 Distrobox 各通过 protocol/直方图两项 CTest；直方图覆盖整数极值、量化误差和百分位 rank。
- 原生 `tests/latency/test_software_metrics.py`：乱序 fragments、双流、真 VAAPI、计时开/关都各解出 12 帧/路；
  开启时全部帧有时码关联，测到约 20 ms 的人工注入周期对应的 parser 等待。
- 真实 720p 回放 6000 AU、零 UDP/解码错误。

```sh
# 编译环境仍分开；不会覆盖归档基线
distrobox enter aura-sdk -- ./tools/build-aura "$PWD/build/aura-metrics"
cmake -S . -B build/host-metrics -DCMAKE_BUILD_TYPE=Release \
  -DTELEOP_BUILD_AURA_SENDER=OFF -DTELEOP_BUILD_HOST_VIEWER=ON \
  -DTELEOP_BUILD_TESTS=ON
cmake --build build/host-metrics
ctest --test-dir build/host-metrics --output-on-failure
python3 tests/latency/test_software_metrics.py
```

板子恢复后，保留旧 `/userdata/teleop-sender`，另部署 `/userdata/teleop-sender-metrics`。
先启动主机 receiver、确认 UDP 端口已绑定且进程仍在，再运行板端短测试。
例如 receiver 等待时间用 60 秒，避免准备另一终端时就超时退出：

```sh
TELEOP_METRICS=1 ./build/host-metrics/bin/host-viewer \
  --port 5000 --headless --frames 300 --timeout-ms 60000

# 另一终端：先做 camera0 的 300 帧单路测试
ssh root@192.168.1.10 \
  'TELEOP_METRICS=1 /userdata/teleop-sender-metrics --camera 0 \
   --width 1280 --height 720 --fps 100 --wrap-lines 0 --frames 300'
```

随后单独 camera1（receiver 加 `--dual-stream` 才接受 stream1），再做两路：

```sh
TELEOP_METRICS=1 ./build/host-metrics/bin/host-viewer \
  --port 5000 --dual-stream --headless --frames 600 --timeout-ms 60000

ssh root@192.168.1.10 \
  'TELEOP_METRICS=1 SENDER=/userdata/teleop-sender-metrics \
   REPORT_DIR=/userdata/latency-metrics-short /userdata/run-ar0234-stereo --frames 300'
```

短测试通过再逐步延长，另起只读 SSH 监控保存 uptime、proc CPU/线程状态、UDP/NIC 计数和温度。
先取得相同程序/设置下可信的实机基线，才比较裁剪、IRQ、频率、缓冲等改动。
纯内核 A/B 应两边使用同一个计时版程序，不把加入测量代码的差异算成内核收益。

原始日志和回放注入脚本位于本机忽略目录 `logs/latency/B0-software-20261012/`。
本轮计时版构建 hash：

```text
aura-sender: 53eaae2b0fb6d37c65a1a96a85fd4b8cf9613055d7d6d9f6ff2a8225f12d93f3
host-viewer: 2c084d330a2ade755f207d429c28af43b75cf618aceb7bb4df04d0de55eb988c
```

新的板端计时尚未实机执行；基线双路编码失联原因与长时稳定性仍待排查。
