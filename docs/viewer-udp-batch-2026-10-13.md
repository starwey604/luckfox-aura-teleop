# UDP 非阻塞批量收包：实现与实机对比

本轮只改 CachyOS 本机 viewer，没有修改 sender、SDK、镜像或网络配置。
默认异步路径使用 `--rx-batch-size 16`；大小 1 保留原有 `poll + recvmsg` 对照。
可选范围 1..32。`--pipeline sync` 仍然逐包接收；文件输入不使用 UDP 批量路径。

## 接收策略

有报文时用 `recvmmsg(MSG_DONTWAIT, timeout=nullptr)` 一次读取当前可用报文，最多 N 个。
不等待凑满批量，不使用延时合并定时器，也不等待下一视频帧。
满批时继续读取；不足 N 个说明这次读取已到达空队列，回到 poll。
每轮 drain 最多读取 256 个报文，批次之间检查取消状态，再回到含 eventfd 的 poll，
避免持续流量妨碍窗口关闭或信号退出。

每批逐包校验、重组；某路 AU 一完成就入该路解码队列。
有限帧模式到达总 AU 数后停止处理，不把一整批当成一个 AU。
保留编码队列容量 4、溢出明确报错及已解码最新帧 mailbox 的策略。

接收 buffer、iovec、mmsghdr、每包 ancillary buffer 均复用。
每次调用恢复 `msg_controllen`、清空返回 flags/length，独立处理每包时间戳、
`SO_RXQ_OVFL` 和 `MSG_TRUNC/MSG_CTRUNC`；部分批量和 EAGAIN 不复用旧结果。
零长度 UDP 报文也算收到一个报文，随后由协议校验拒绝。

批内共用 syscall 返回时采集的 MONOTONIC 用户接收时间；内核 REALTIME RX 时间戳仍逐包独立。
整批只读取一次 REALTIME/MONOTONIC 校准对，保持同域年龄计算与时钟跳变拒绝规则。
因此 `reassembly` 的用户接收起点在批内更粗，不能把它单独作为整体链路改善依据。
内核 RX→用户指标保留逐包样本；批内越早到达的包，其年龄通常越大。

新增退出统计：

| 输出 | 含义 |
|---|---|
| `rx-syscalls.calls` | 接收 syscall 尝试次数，含 EAGAIN 等失败。 |
| `successful_calls/datagrams` | 成功调用次数/从 socket 实际读出的报文总数，包含之后协议拒绝的报文。 |
| `max_batch/mean_batch` | 实际一次成功调用的最大/平均报文数，不是配置上限。 |
| `would_block` | 非阻塞读取遇到空队列的次数。 |
| `rx-thread.batch_capacity/poll_calls` | 配置上限和 poll 调用次数。 |
| `rx-thread.cpu_ms` | RX 线程从进入接收函数到结束的 CLOCK_THREAD_CPUTIME_ID 增量；包含重组/观测/交接初始化和工作，不包含后续直方图打印。 |
| `host.rx.batch_process` | 批量路径处理一批返回报文的本地 CPU 调用耗时直方图；需要 TELEOP_METRICS=1。 |

CPU 时间只覆盖收包/重组线程，阻塞等待不计 CPU；不能用测试脚本总耗时作其分母。
对比时以实际报文数量归一化，避免相机内容改变压缩码量造成误判。

## 使用与测试

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/build-host
TELEOP_METRICS=1 ./build/host/bin/host-viewer \
  --dual-stream --frames 0 --timeout-ms 3600000 \
  --window-width 1080 --window-height 960 --borderless \
  --rx-batch-size 16 --queue-report-ms 1000
```

日常使用可以省略 `--rx-batch-size 16`，关闭详细统计时去掉 `TELEOP_METRICS=1`。
用 `--rx-batch-size 1` 在相同异步架构下比较逐包路径。

原生构建零警告。14 项 viewer 测试、2 项已有 VAAPI 时间关联测试、3 项 CTest 全部通过。
新增 CTest 使用真实本地 UDP socket 验证：空队列立即 EAGAIN、1/3/32/7 个预入队报文、
部分批量不等待、payload 顺序、复用后所有包时间戳、超长/零长度报文，以及小 SO_RCVBUF
造成真实溢出后下一报文携带的 SO_RXQ_OVFL。故障测试中的溢出是刻意触发。
新增 viewer 回归覆盖 1/8/16/32 各档双路 VAAPI 与逐包时间戳，并验证繁忙接收队列下 SIGINT 退出。

```sh
cmake -S . -B build/host -DTELEOP_BUILD_TESTS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 tests/latency/test_viewer_pipeline.py
TELEOP_TEST_VIEWER=build/host/bin/host-viewer python3 tests/latency/test_software_metrics.py

./tools/test-viewer-rx-batch --output logs/viewer-rx-batch/new-run
./tools/test-viewer-rx-batch --rounds 1 --minimal --no-display \
  --output logs/viewer-rx-batch/minimal-run
```

实机工具默认交错做三轮 1/8/16/32 headless，再各档做一次真实窗口。
默认保持详细阶段观测；`--minimal` 关闭阶段统计和板端轮询，接收计数/线程 CPU 时间仍保留。
SSH 认证沿用既有配置，支持 SSHPASS 环境变量；不在文件中保存密码。
`tools/test-aura-software-baseline` 也支持 `--rx-batch-size`。
既有 `test-viewer-pipeline-ab` 强制批量大小 1，使解耦架构对照保持单一变量。

## 实机数据

原 A 内核 `6.1.141 #16 SMP PREEMPT`，boot ID 始终为
`32d05d60-7683-4865-868e-866f1a7217b9`。sender/launcher 哈希保持上一轮不变。
两颗 AR0234，1280×720，约 99.23 fps，30 Mbps、GOP15、整 AU mode1、wrap=0。
每次每相机固定 120 帧，启动温度阈值 70℃；独立 USB kmsg 先收到 live marker 再启动相机。

三轮 headless，共 12 次短测，详细统计开启：

| 上限 | 平均接收调用/次测试 | 实际平均批量 | RX CPU ms/次测试 | RX CPU μs/报文 | RX 年龄均值 μs | 各轮 p99 范围 μs |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 7309 | 1.00 | 13.163 | 1.801 | 8.04 | 19.97–34.82 |
| 8 | 3076 | 2.37 | 11.952 | 1.637 | 7.95 | 22.53–47.10 |
| 16 | 3168 | 2.33 | 10.051 | 1.363 | 7.48 | 18.18–31.74 |
| 32 | 3209 | 2.31 | 10.304 | 1.392 | 7.66 | 23.55–32.77 |

RX 年龄均值按报文样本加权；p99 是各轮范围，没有伪造合并分位数。
16 档每包 CPU 成本约下降 24.3%，接收调用约下降 57%。实际每批约 2.3 个包，
32 上限并没有让它变成每次 32 包；这取决于到达节奏和调度。
16 的 headless CPU 均值最低，p99 没有明显变差，因此选为默认；三轮不足证明它在所有负载下最优。

关闭详细观测的补充 headless 短测，各档 1 次：

| 上限 | 接收调用 | RX CPU μs/报文 |
|---:|---:|---:|
| 1 | 7386 | 1.734 |
| 8 | 3133 | 1.480 |
| 16 | 3078 | 1.453 |
| 32 | 3208 | 1.537 |

16 档降低约 16.2%，说明收益不完全来自批量减少时间戳/直方图开销。
这组只有一次，且温度、CPU 频率、场景内容等未被严格固定，作为方向验证。

各档真实窗口各 1 次全部通过：每路 120 硬解帧，最后显示序号均为 120；
RX 年龄最大值分别约 0.146/0.140/0.182/0.099 ms。窗口改变相机所见内容，
各次报文数不同，单次窗口 CPU 值不用于选默认档位。

全轮 **20 次短测 / 4800 硬解帧**：零 malformed、重组丢帧、RX socket 溢出、编码队列溢出；
每个带观测的实际读取包都有独立内核时间戳，未检测到时钟跳变。
USB marker 后没有 BUG/WARNING/软锁/硬锁，板子没有重启或失联。
最终温度约 44.88℃，测试进程已退出。

本轮主要降低系统调用与 RX CPU 成本；正常 RX 等待本来就是几微秒，
没有测得毫秒级端到端改进，也没有测 screen-to-screen 或更高码率/负载下的长期 p99。

机器可读摘要：`docs/baselines/2026-10-13-viewer-udp-batch.json`。
原始日志留在忽略目录 `artifacts/viewer-batch-20261013/`，没有提交二进制或抓图。
