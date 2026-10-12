# Sender 初始化与资源回收排查（2026-10-12）

后续 #14 实机、CPU0 hardlock 证据和待烧写 #16 镜像见
[下一轮排查记录](sender-hardlock-debug-2026-10-12.md)。以下保留初轮测量条件和历史结论。

本轮实机只替换 `/userdata` 应用，保持旧内核 #12 PREEMPT、设备树、两路 AR0234
1280×720 ~99.23fps 和编码参数不变。另做了临时 VBLANK 对照，已恢复原值。为进一步排查已编译诊断镜像，尚未烧录。未做 screen-to-screen 测量。
测试严格使用每路 120 帧，启动温度 <50°C；不做自动长时间负载测试。

## 已确认的程序问题与修复

- 原构造函数边初始化边申请资源，中途抛异常不会调用本对象析构函数，已成功的 VI/VENC 无人回收。
  现在先将空 pipeline 放入持有者，再调用 initialize；异常与正常路径都在 join 所有工作线程后回收部分资源。
- 原码流需要手动 ReleaseStream；分配内存、输出文件、处理 pack 等抛异常会遗留一份已取得的码流。
  现在用局部 RAII lease 保证一次释放，先释放再回收编码器，释放失败也报告错误。
- 原两个进程各自 SYS_Init/SYS_Exit，使同一全局 MPI 系统的生命周期交错。
  新增 `--dual`，一个进程、一次 SYS_Init、两路独立 UDP sender/编码线程，最后一次 SYS_Exit。
  `/tmp/teleop-mpp.lock` 使用 flock 拒绝第二个新 sender；文件存在本身不表示占用，进程结束会释放锁。
  老版本 sender 不遵守此锁，仍须避免新旧程序混跑。
- 一路达到有限帧数后继续取并释放后续码流，直到另一侧也完成，避免提前停止消费造成共享 ISP/VENC 反压。
- 所有工作线程退出后，先停止两路绑定和接收，再 DestroyChn/DisableChn/DisableDev；
  按 RV1126B sample 增加 SYS_WaitFreeMB，然后 SYS_Exit。每步返回值可见，清理失败返回非零。
- 信号停止标记改为 lock-free atomic，metrics 改为线程独立直方图，双路指标加 stream0/stream1 前缀。

参考 SDK：`media/samples/example/common/sample_comm_vi.c`、`sample_comm_venc.c`、
`media/samples/example/demo/sample_demo_dual_camera.c`，接口为 `media/out/include/rk_mpi_sys.h`。
官方双摄 sample 在所有 VI/VENC 配置完成后才做 SYS_Bind。

## 初始化顺序的新证据

第一个单进程候选仍是 camera0 完成绑定、启动取流线程，再初始化 camera1。
它在同一开机状态下连续完成 4 次双路、2 次单路短测试（每路 120 帧），
每次接收和 VAAPI 解码帧数完整、UDP malformed/duplicates/dropped 都是 0，回收 API 都成功。
每次 CmaFree 都从 278448 kB 恢复到 278448 kB，CmaAllocated 恢复到 80 kB，MPP session 清空。

但是随后的 camera1.vi.enabled 故障注入测试在到达注入点前就使整板失联：
最后 camera1 日志是 `BEGIN RK_MPI_VI_EnableChn`，没有 END；camera0 最后是
`BEGIN RK_MPI_VENC_ReleaseStream`，同样没有 END。尚未执行故障注入后的回收。
因此不能认定只是析构或过热问题，也不能将前 6 次通过当成稳定性证明。
高频 GetStream/ReleaseStream 日志在这个测试中启用，调度扰动也是一个变量。

第二个候选按官方 sample 调整：**所有 VI/VENC 配置完成 → 两路绑定 → 启动两个工作线程**。
这避免第二路 EnableChn 改变共享 ISP 状态时第一路正在编码/释放码流。
自动测试加入“不允许已绑定编码时继续 EnableChn”检查。
此处是依据日志和 sample 的修复假设，尚不能证明 SDK 内部具体锁或 IRQ 是根因。

## 插桩与复现

```sh
# 板端，先做有限帧数验证；不要另开一个 --camera 1 进程
AURA_LIFECYCLE_TRACE=1 /userdata/run-ar0234-stereo --frames 120

# 需要 pack 获取/释放的细日志时才打开，影响时序且输出量大
AURA_LIFECYCLE_TRACE=1 AURA_TRACE_STREAM=1 \
  /userdata/teleop-sender --dual --width 1280 --height 720 --wrap-lines 0 --frames 120

# 初始化/资源获取后的异常注入，预期退出码 1
AURA_LIFECYCLE_TRACE=1 AURA_FAIL_AFTER=camera1.venc.created \
  /userdata/teleop-sender --dual --width 1280 --height 720 --wrap-lines 0 --frames 120
```

可注入阶段：`vi.device`、`vi.enabled`、`venc.created`、`venc.started`、`bound`、
`stream.acquired`，可加 `camera0.` / `camera1.` 前缀。不要在正常运行时保留 AURA_FAIL_AFTER。
双路录像传 `--output-h264 '/userdata/camera{camera}.h264'`，脚本 H264_DIR 自动传入这个模板。

主机原生测试：

```sh
./tools/test-aura-lifecycle --output logs/lifecycle/<new-run>
# 只做故障/信号/锁测试，最后仍做一次双路 VAAPI 恢复测试
./tools/test-aura-lifecycle --skip-stream-tests --output logs/lifecycle/<new-run>
```

测试套件先做双路→单路0→单路1→双路，随后注入第二路 VI/VENC 初始化失败及取得码流后失败，
测试初始化中 SIGTERM、出流后 SIGINT、第二进程互斥，最后重跑双路解码验证恢复。
每个相机运行均有有限 120 帧及外层超时；超时升级 KILL 只用于测试兜底，不是正确回收的证明。
SSH 不响应或任一验证失败立即停止后续测试，由用户手动重启。

`test-aura-software-baseline` 会保存 API 日志、实时 `/dev/kmsg`、每线程 wchan/stack、温度、
CPU/网络统计、MPP session 和 MB pool 快照。故障套件单独通过第二条 SSH 保存 kernel-live.txt，
避免 eMMC 日志尚未落盘就失联。真正 IRQ/总线硬锁仍可能来不及通过网络输出日志。

## 构建与离线测试

```sh
distrobox enter aura-sdk -- ./tools/build-aura
# 原生 MPI fake 测试也在 Ubuntu Distrobox 编译，仅依赖 SDK 头文件
distrobox enter aura-sdk -- cmake -S . -B build/lifecycle-tests \
  -DTELEOP_BUILD_AURA_SENDER=OFF -DTELEOP_BUILD_TESTS=ON \
  -DTELEOP_BUILD_MPI_LIFECYCLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
distrobox enter aura-sdk -- cmake --build build/lifecycle-tests
distrobox enter aura-sdk -- ctest --test-dir build/lifecycle-tests --output-on-failure
```

fake 测试运行真实 sender 实现，检查 6 个资源获取检查点、13 种 API 错误、第二路部分初始化、
回收错误状态、双路有限帧数的持续排空、统一 SYS 生命周期及 flock。它只能验证程序资源管理，
不能替代 SDK 内核、DMA/ISP/VENC 的实机稳定性测试。脚本另有 7 个离线 launcher 测试。
主机 protocol/performance 和双路 UDP→VAAPI metrics 测试已通过（每路 12 帧硬解）。
Distrobox 下 ASan+UBSan 的 protocol/performance/MPI lifecycle 三项测试全部通过。

## 实机验证结果与限制

分阶段初始化候选已在 Distrobox 交叉编译，SHA256：
`36468d1e7192db7274718b181fb20d631ee27fbeed367d9c96f0f1f706cf07aa`。
同一次开机先后进行了：

| 测试 | 结果 |
|---|---|
| 双路→单路0→单路1→双路 | 4 次通过，每路 120 帧全部接收/VAAPI 硬解，无 UDP 错误 |
| camera1.vi.enabled / camera1.venc.created / camera0.stream.acquired 注入 | 3 次预期退出码 1；释放成功、资源恢复；没有卡死 |
| 初始化中 SIGTERM、ready 后 SIGINT | 2 次成功回收 |
| ready 后另起第二个 sender | 被 flock 拒绝，没有第二次 SYS_Init |
| 故障后双路恢复 | 通过，每路 120 帧硬解 |
| 随后连续双路重复启动 | 前 4 次通过；第 5 次开始出流后整板失联 |

最终一次失联前，两路 EnableChn / VENC / SYS_Bind 均已返回成功，尚未进入退出回收。
最后 CPU 温度 49.94°C、VENC 550MHz、进程 RSS 7568kB；日志尚无 panic/调用栈。
这说明用户态资源回收修复和初始化顺序调整仍不足以消除底层失联。
成功测试结束均恢复 CmaFree=278448kB、CmaAllocated=80kB，MPP session 清空；暂未看到累积 CMA 泄漏。
线程 wchan 有数据，`/proc/PID/task/TID/stack` 在现有内核不存在（CONFIG_STACKTRACE 未启用）。

随后从冷启动临时对两路设 `vertical_blanking=80`，扩大 blank 到约 1088µs：
HTS=2448、pixel rate=180MHz，周期=2448×(720+80)/180MHz≈10.88ms，约91.91fps。
CIF 的 `capture.c:rkcif_check_vblank_value` 要求在线 ISP blank≥1000µs，原值21行仅285.6µs。
此条件下 4 轮双路短测通过；第 5 轮温度保护拒绝启动，没有发生失联。
控制值已经恢复21，两路枚举和 SSH 正常。这个小样本不能证明 blank 是根因，也不能说明100fps已稳定。
采集速率改变，性能比较必须分组，不能把约92fps与原99.23fps混为一组。

原始日志（git ignore）：
`logs/lifecycle/20261012/`，包含 staged-init、staged-faults、restarts、vblank80。
程序仍为 `/userdata/teleop-sender-lifecycle`，launcher 为 `/userdata/run-ar0234-stereo-lifecycle`；
尚未覆盖板端默认旧程序，以免把未通过稳定性验证的版本误当正式版。
本机标准 `build/aura/bin/aura-sender` 已更新到这个候选。

## 已构建的诊断镜像：需要用户烧写后继续

新增可选 profile `ar0234-stereo-debug`。kernel 第17条、BoardConfig 第9条 patch 已保存，
干净旧 SDK 重放得到相同源码树；诊断选项不影响其他 profile。
诊断 fragment 在既有 PREEMPT/HZ1000/O2 和 AR0234 基础上启用：

- STACKTRACE、KALLSYMS_ALL：读取等待线程内核栈并保留符号。
- DETECT_HUNG_TASK，超时10秒：打印卡在 D 状态的任务。
- NETCONSOLE/NETCONSOLE_DYNAMIC：通过独立 UDP 端口直接接收 kernel printk，减少对 SSH 的依赖。
- soft/hard lockup、hung-task、RCU stall 的默认 panic 关闭，保留检测与日志。
  原板 watchdog=1、watchdog_thresh=10，hardlockup_panic=1、softlockup_panic=1；有报警可能直接进入 panic。

保持锁结构相关配置原状，未开启 lockdep/DEBUG_SPINLOCK：SDK Rockit 部分仅提供预生成汇编，
不能假设改变这些结构布局后仍兼容。诊断版本不用于延迟 baseline。
目前仍没有真正的 ramoops DT reserved-memory 后端，不能期待断电重启后保留日志。

构建与校验已完成（Ubuntu Distrobox）：

```sh
./tools/select-teleop-board ../aura-sdk ar0234-stereo-debug
distrobox enter aura-sdk -- bash -c 'cd /home/ww/codings/aura-sdk && ./build.sh driver && ./build.sh firmware'
```

`driver` 同时重建 kernel 和外部模块，再 firmware 打包；没有仅换 boot 保留旧外部模块。
当前可烧写文件在 `/home/ww/codings/aura-sdk/output/image`；独立备份在
`artifacts/lifecycle-debug-20261012/image`，同时保留 config、System.map、vmlinux、校验和。
内核为 #14 SMP PREEMPT 2026-10-12 18:51:27 CST；5个关键MPP/Rockit模块的vermagic均匹配6.1.141 SMP preempt。
[镜像与配置 SHA256](baselines/2026-10-12-lifecycle-debug.json) 已提交，boot.img 为11,389,440字节，未超11MiB分区。
所有定长镜像大小均未超对应分区；全量烧录脚本 dry-run、runtime check、SDK replay 和8项patch工具测试通过。
**没有自动烧录、重启或清空板端 userdata。全量烧写会覆盖 userdata；烧后需重新部署 sender 和相机脚本。**

进入 USB Loader 后由用户执行：

```sh
cd /home/ww/codings/luckfox-aura-teleop
./tools/flash-aura-emmc
```

烧写并恢复 SSH 后，先部署本轮候选，再在 CachyOS 开独立终端采集网口内核日志：

```sh
./tools/deploy-aura
./tools/capture-aura-kernel-log --output logs/lifecycle/netconsole-<new-run>.txt
```

receiver 会自动读取 enp195s0 的 MAC，绑定192.168.1.2:6666，然后通过 SSH 建临时netconsole目标。
收到自测包才确认ready，Ctrl+C移除自己建立的目标并恢复console loglevel；不会启动相机。
该工具已做 dry-run/前端回环测试，板端NETCONSOLE还需新镜像实测。
再用 `test-aura-lifecycle` 和有限120帧重复启动，收集失联前stack、hung-task、lockup信息。
如果网卡/所有CPU或总线一起硬锁，netconsole也可能没有最终日志，不能承诺必获dump。

性能比较需先固定通过生命周期验证的新应用版本作为软件 baseline；不能将旧两进程 B0 与新应用加新内核的差值归因于内核。
旧 B0 镜像/二进制和原始记录继续保留；复跑旧流程应使用对应历史提交的工具。
