# Viewer 解耦与队列观测：实现及实机结果

本轮只更新 CachyOS 本机程序，没有修改、部署 sender 或刷写镜像。默认 UDP viewer
已采用 `--pipeline async`，可用 `--pipeline sync` 切回同步路径做对照。
显示卡顿不会再让同一个线程停止收包和解码；这不等于消除了显示本身的卡顿。

## 执行路径与容量

- 一个 RX 线程负责 `poll + recvmsg`、报文校验、每路 AU 重组。保留完整 AU 直接解码和旧 Annex-B parser。
- 每路独立解码线程，各自持有 FFmpeg/VAAPI context、parser 和阶段直方图。
- 每路编码队列默认最多 4 个待解码 AU，`--decode-queue-capacity 1..64` 可调整。
  超出容量会输出明确错误并停止整个 viewer；**不任意丢弃压缩 P 帧**，本轮还没有实现等待 IDR 自动恢复。
  容量只约束待解码 AU，不包括当前解码 AU、重组缓存、内核 socket 或 FFmpeg 内部缓冲。
- 每路已解码帧使用容量 1 的 mailbox。显示忙时替换尚未取走的旧帧，保留最新 `AVFrame` 引用。
  主线程批量取当前有更新的各路，再渲染一次窗口；没有更新的一路保留最后画面，不等待另一相机。
- SDL 事件、Vulkan、libplacebo 始终由主线程操作。VAAPI frame 引用跨线程传递，释放旧 mailbox 引用时不持队列锁。
- 正常有限帧结束：停止 RX、处理已入队 AU、flush decoder、提交最后 mailbox、join 各线程。
  关闭窗口、SIGINT/SIGTERM、超时或任一线程错误：通知所有线程取消，用 eventfd 唤醒 RX 的 poll，再 join。
  驱动调用本身若永久阻塞，eventfd 不能中断它；本轮验证的是正常驱动下的取消和异常路径。

`--input FILE` 保留原来的同步文件解码路径，不经过 UDP 异步队列。
没有引入 `recvmmsg`、busy-poll、实时调度或新的编译优化，后续可单独测试这些变量。

## 使用与观测

在本机仓库目录原生构建并启动：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/build-host
TELEOP_METRICS=1 ./build/host/bin/host-viewer \
  --dual-stream --port 5000 --frames 0 --timeout-ms 3600000 \
  --window-width 1080 --window-height 960 --borderless \
  --queue-report-ms 1000
```

sender 继续沿用 `/userdata/run-ar0234-stereo`。窗口仍为上下两路，保留现有尺寸及双路边框处理。
日常关闭阶段统计时去掉 `TELEOP_METRICS=1`；实时队列输出默认关闭，去掉 `--queue-report-ms` 即可。
不使用软件解码作为自动降级；`--software-decode` 仅为显式诊断。

| 输出 | 含义 |
|---|---|
| `queue-sample` | 主线程定期快照：当前待解码深度/字节、峰值、最老 AU 等待、mailbox 是否有帧及其年龄、已解码/替换计数。打印在锁外并 flush。显示阻塞期间快照可能延迟。 |
| `queue` | 退出汇总：入队数量、峰值深度/字节、编码溢出、mailbox 替换、提交显示数量、最后解码/显示序号、剩余队列。序号是本 viewer 的解码计数，不是 sensor 帧编号。 |
| `host.kernel_rx_to_user` | 内核软件 RX 时间戳到 `recvmsg` 的同域时间差，包含主机网络栈/socket 排队和调度等待；按报文统计。 |
| `rx-observer` | 时间戳/溢出功能是否启用、时间戳样本、`SO_RXQ_OVFL` 累计、截断、时钟异常、RX 年龄最大值。溢出累计只有后续报文携带 cmsg 后才能更新。 |
| `host.streamN.au_queue_wait` | AU 重组完成到解码线程取出该 AU，包含入队/唤醒等待。 |
| `host.streamN.au_ready_to_frame` | AU 完成到 FFmpeg 返回帧，包含队列和解码调用；硬件帧返回不保证 GPU 工作完成。 |
| `host.streamN.mailbox_publish` | 解码帧 clone/引用交接耗时。同步对照仍输出 `present_call`。 |
| `host.streamN.decoded_to_present_begin` | 被显示的更新帧：解码返回后到主线程开始本次显示调用。被替换帧不进入此统计。 |
| `host.streamN.au_ready_to_present_return` | 被显示的更新帧：AU 完成到显示调用返回。不是屏幕出光时刻。 |
| `host.display.*` / `host.streamN.map_call/render_call/unmap_call` | swapchain 获取/提交/swap 及各路映射、渲染、取消映射的 CPU 调用耗时。 |

`SO_TIMESTAMPNS` 使用 CLOCK_REALTIME，只在 `TELEOP_METRICS=1` 开启。
先在 REALTIME 内计算 RX 年龄，再以该时长记录到本地 MONOTONIC 直方图；不直接跨时钟相减。
检测到约 1 ms 以上的 REALTIME/MONOTONIC 偏移跳变或无效年龄时拒绝样本，跳变前积压报文也会被拒绝。
这不是网卡硬件时间戳，不测板端→主机单向时延；小幅校时仍可能影响结果。

`displayed` 可以少于 `decoder: frames`：这是解码后替换旧画面的结果，不能当成编码丢帧。
`presenter: frames` 是整窗提交次数；一次提交可能更新两路，也可能只更新一路。

## 原生回归

本机 GCC Release/O3 构建通过且没有警告。12 项新测试、2 项已有 VAAPI 测试、
2 项协议/阶段统计 CTest 全部通过。

```sh
python3 tests/latency/test_viewer_pipeline.py
TELEOP_TEST_VIEWER=build/host/bin/host-viewer python3 tests/latency/test_software_metrics.py
ctest --test-dir build/host --output-on-failure
```

新测试使用真实 VAAPI/Vulkan，覆盖双路显示阻塞、编码队列溢出、闲置第二路、
无输入关闭窗口、SIGINT、超时、解码错误、初始化错误、AU 合约变化、截断报文、
软件帧显示，以及已获取 swapchain 后注入异常的资源回收。

显示阻塞实验：两路各 60 帧、100 fps，每次显示额外等待 100 ms。
最终回归的内核 RX 年龄最大值为同步 **12395.87 ms**、异步 **0.189 ms**；
两路均完整硬解 60 帧、无网络/编码队列溢出，异步最后显示序号都为 60。
这是验证阻塞隔离的故障注入，不是正常链路延迟估计。

补充测试暴露了原有软件解码显示路径缺少 libplacebo backing textures 的问题。
现在每路有独立、可复用的 4 平面纹理；异常时也会关闭已获取的 swapchain frame，
避免 teardown 等待未提交的 GPU 工作。硬解仍沿用 VAAPI→DRM PRIME→Vulkan。

## 板端短测 A/B

运行的是原 A 内核 `6.1.141 #16 SMP PREEMPT`，boot ID 始终为
`32d05d60-7683-4865-868e-866f1a7217b9`，两颗 AR0234，1280×720，约 99.23 fps，
双路 wrap=0、30 Mbps、GOP15、整 AU mode1。没有改变内核、网络参数、sensor 时序或编译选项。

先用同一个 viewer 二进制交替运行同步/异步 headless 各 3 次，再测试同步/异步真实窗口。
每次每相机只采 120 帧，启动前检查温度，使用独立 USB `/dev/kmsg` 通道并先确认 live marker。
随后两次窗口对照验证日志补充，再两次验证最终发布二进制，共 **12 次短测 / 2880 硬解帧**。
全部成功，零重组丢帧、零 malformed、零 RX socket 溢出、零编码队列溢出；
USB marker 后没有 BUG/WARNING/软锁/硬锁。最终约 44.94℃，没有遗留 sender、viewer 或本轮 USB reader。

headless 三次样本加权均值，单位 ms：

| 本机阶段 | sync | async |
|---|---:|---:|
| 内核软件 RX→应用 | 0.03970 | 0.02461 |
| stream0 AU 完成→返回帧 | 0.34133 | 0.42450 |
| stream1 AU 完成→返回帧 | 0.42699 | 0.46880 |
| stream0 队列等待 | 无独立队列 | 0.02167 |
| stream1 队列等待 | 无独立队列 | 0.02493 |

独立线程增加了少量交接成本：headless 解码阶段均值没有变快。
本轮主要收益是避免显示阻塞造成两路接收积压，不宣称所有阶段都下降。

最终发布二进制的真实窗口对照：

| 指标 | sync | async |
|---|---:|---:|
| 最长显示调用 ms | 124.28 | 127.14 |
| 内核 RX 年龄最大值 ms | 132.92 | 0.157 |
| 内核 RX 年龄平均值 ms | 10.138 | 0.00767 |
| 硬解帧，stream0 / stream1 | 120 / 120 | 120 / 120 |
| 编码队列峰值，每路 | 无独立队列 | 1 / 1 |
| mailbox 替换，每路 | 不适用 | 12 / 12 |
| 更新帧提交显示，每路 | 120 / 120 | 108 / 108 |
| 最后显示序号，每路 | 120 / 120 | 120 / 120 |
| 整窗提交 | 240 | 214 |

两种模式均出现首次渲染长调用；异步情况下 RX 和解码继续前进，旧画面被最新帧替换。
这是两次短样本，不能据此给出长期 p99 或 screen-to-screen 改善值。

复测入口（原生 CachyOS，SSH/ADB 均已连接）：

```sh
./tools/test-viewer-pipeline-ab --output logs/viewer-pipeline/new-run
# 只做两个真实窗口短测：
./tools/test-viewer-pipeline-ab --headless-pairs 0 --output logs/viewer-pipeline/display-run
```

工具支持既有 `SSHPASS` 环境变量；不设置时使用 SSH 认证。禁止自动烧录/重启，
每路固定 120 帧；USB live marker、boot ID、每轮温度和 sender/viewer 退出均受检查。
`tools/test-aura-software-baseline` 也新增 `--viewer-pipeline sync|async`、`--display`、`--queue-report-ms`。

机器可读记录保存在 `docs/baselines/2026-10-13-viewer-pipeline.json`。
原始日志在本机忽略目录 `artifacts/viewer-pipeline-20261013/`，不提交生成物或抓图。
后续优先单项尝试 RX 批量读取，再测线程亲和性或编译参数；保留此轮队列指标作为对照。
