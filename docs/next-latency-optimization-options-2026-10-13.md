# 下一轮延迟优化：实际路径、候选与验证顺序

2026-10-13。依据 K1 的 [A→B→A-return](kernel-k1-ab-2026-10-13.md)、程序源码、
板端 SSH 只读检查、本机 NIC/CPU/GPU 状态和隔离编译探针。
本轮没有裁剪新的镜像、修改生产代码、调整 governor/IRQ/NIC，或部署实验二进制；没有启动实机摄像头。

推荐顺序：**先让 viewer 显示不阻塞收包，再补队列观测与优化 socket，随后单项测编译选项，
继续裁剪后台/无线/显示/NPU，最后研究真正的 slice 流水与内核编译器变更。**
现有均值已经是几十到几百微秒，不能保证 O3/LTO/裁驱动还能省出数毫秒。
可见显示验证中一次136ms提交长尾；阻塞传播和积压比微小CPU算力变化更值得先处理。

## 1. 已核实的瓶颈与观测盲区

`src/host-viewer/src/main.cpp` 每个报文都做一次 `poll + recv`；完整AU到达后同步调用解码。
`h264_decoder.cpp::drain()` 在同一调用链执行 sink→present。
`vulkan_presenter.cpp::present_stream()` 每到任一路的一帧，都会重画两路最新帧并提交整窗。
双路100fps因此可能发起约200次整窗重画；它不是200Hz显示，且一侧旧画面反复被导入和重画。
单次慢的 map/render/swap调用会同时阻塞两路收包、解码。

当前 `host.reassembly` 从**用户空间收到第一个分片**开始计时，漏掉该分片在socket中已经等待的时间。
`submit_to_frame`只测到VAAPI frame对象返回，未确认GPU完成；不能用这两个短数字证明没有显示积压。
136ms长尾也还没有细分原因，不能直接认定一定是shader编译或VSync。

sender已用`sendmmsg`、整AU立即发送，viewer已绕过整AU的parser lookahead；
先前两处额外帧边界等待已消除，不能再次当作新优化的收益。
K1裁剪主要带来0.91MiB镜像与1596KiB可管理内存收益，尚未证明整体延迟收益。

## 2. Viewer：最高优先级，保留现有库与上下布局

建议四条应用执行路径：一个RX/重组线程、每路一个解码线程、SDL/Vulkan主线程。
SDL事件与Vulkan对象都留在主线程，每个FFmpeg context只由所属解码线程操作。

- RX用`poll`等待可读，再以`recvmmsg(MSG_DONTWAIT)`取当前已有的16/32个报文，读空后才回到poll。
  不为了凑满batch或两路帧而等待；`MSG_WAITFORONE`可作另一对照，不能误用普通阻塞batch。
  这是减少syscall/调度干扰的候选，尚无实测节省值。[recvmmsg手册](https://man7.org/linux/man-pages/man2/recvmmsg.2.html)
- 每路编码AU队列有界，先试2/4个元素并记录深度、最老AU年龄和溢出。
  正常情况下尽快按序解码，不能为“最新帧”任意丢H.264参考P帧。
  若确实跟不上，明确丢弃损坏GOP、flush并等待/请求IDR，记录恢复耗时，不静默显示花屏。
- 解码输出每路一个latest mailbox，覆盖的是**已解码**旧显示帧，保留并正确释放AVFrame引用。
  显示只取当前可用最新帧，不等待另一相机达到同帧号；相机仍是异步曝光，不声称硬件同步。
- 合成两路时只对可用更新做一次整窗提交，按present能力取最新画面。
  不加固定10ms“拼帧等待”，也不让100fps捕获被限制成较低的解码率。
  继续用旋转副屏的逻辑上下布局。
- 有限帧结束必须先正确停止RX、drain两路编码队列、flush decoder、释放mailbox并join线程；
  不能因达到接收帧数就丢掉最后的硬解输出。TLS指标也要从各工作线程汇总，避免计时搬线程后丢样本。

先拆出`rx_wait/recv_batch、kernel_rx_to_user、AU_queue_wait、decode_submit、map、render、swapchain_wait/submit/swap`。
增加每路解码/显示序号、显示替换计数、队列峰值，以及主线程最久未收包的间隔。
主机RX软件时间戳和SO_RXQ_OVFL由recvmsg/recvmmsg的cmsg取得；处理truncation、计数回绕。
软件RX时间戳通常使用实时钟，现有指标是MONOTONIC：要明确时钟域、用同域时间计算或校准转换并排除跳变，
不能直接相减。现有板/主机capture timestamp也仍不能直接当作单向网络时间。
[socket选项](https://man7.org/linux/man-pages/man7/socket.7.html)，
[Linux时间戳说明](https://docs.kernel.org/networking/timestamping.html)。

### 渲染缓存与预热

当前请求MAILBOX、swapchain_depth=1，并使用libplacebo fast参数，已经有降低排队的意图。
depth控制GPU在途工作，不保证只有一个物理swapchain image或没有KWin排队；记录实际GPU/呈现模式并单独比较depth1/2。
本机VAAPI节点为AMD `0000:c5:00.0`，应记录Vulkan实际选卡，确认互操作设备匹配。
[libplacebo Vulkan接口](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/vulkan.h)

本程序未显式设置`pl_gpu_set_cache`。安装的libplacebo7.360.1支持`pl_cache`加载/保存，
可在创建渲染pass之前附加缓存，再预热两路实际NV12尺寸/色彩/布局对应的路径。
缓存按GPU/驱动/libplacebo版本管理并原子保存；当前无显式缓存不代表Mesa没有自己的缓存。
分别测空缓存、加载缓存、预热后的首帧与稳态；按细分计时判断是否真的消除了尖峰。
[libplacebo缓存接口](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/cache.h)

缓存/复用VAAPI surface的导入结果可后续尝试，但必须跟踪AVFrame、DMA-BUF及GPU工作生命周期，
不能只按会复用的整数fd做永久缓存。`pl_avframe_params.tex`主要供软件帧纹理复用，
当前硬解路径不是给这个字段填非空值就能避免所有导入成本。

## 3. Sender 与网络：先改善现有 UDP 路径

当前双路约60Mbps，1200字节报文对应约6000多报文/秒量级，远非千兆带宽瓶颈。
优先保持Linux UDP；现在直接移植DPDK/AF_XDP会扩大驱动、UMEM、协议、调度和恢复工作，
本机/板端零拷贝模式是否支持尚未验证，不将其列为第一轮。
[AF_XDP的驱动及copy/zero-copy要求](https://docs.kernel.org/networking/af_xdp.html)

| 候选 | 当前证据与建议 |
|---|---|
| sender mode1消除AU复制 | 当前把MPP整pack insert进vector，再sendmmsg；发送后才ReleaseStream。可在同样的lease作用域直接用span发送，保持IDR识别/异常回收，省一次用户态复制；这不等于内核零拷贝。 |
| viewer复用buffer/AVPacket/AVFrame | 重组vector每帧move走；之后av_new_packet又复制一次。可建有界buffer池、以AVBufferRef接管AU，严格保留FFmpeg要求的尾部padding及引用生命周期。 |
| 去掉报文头重复解析 | main与reassembler各parse一次；允许传已验证header但保留bounds/flags/覆盖完整性检查。先用microbench判断价值。 |
| connected UDP | sender可选connect后无msg_name发送，测路由查找成本；同时测试端口未监听时ICMP/ECONNREFUSED的行为变化，不能破坏无viewer的初始化/回收测试。 |
| socket buffer | 当前请求4MiB，host有效Linux值8MiB；板端上限较小。先增加显式参数、setsockopt错误和overflow/排队年龄，再试256KiB/512KiB/1MiB/4MiB。不能只缩缓冲然后把丢帧叫低延迟。 |
| 主机busy poll | host配置支持NET_RX_BUSY_POLL，sysctl为0；用真实首包读取NAPI ID验证，再单socket试0/25/50us。先分离显示，避免给被显示阻塞的线程加忙等。CPU代价单独记录。[NAPI文档](https://docs.kernel.org/networking/napi.html) |
| IRQ/RPS/线程亲和性 | 板端单RX/TX队列，RPS/XPS都是0，GMAC/CIF/ISP/VENC IRQ计数集中CPU0，允许mask0–3不等于自动分散。先只移动GMAC IRQ，再只移动VENC IRQ，再试sender worker；每组都测初始化/回收，保留KMPP修复与USB日志。[网络扩展机制](https://docs.kernel.org/networking/scaling.html) |

本机`enp195s0`是r8169/RTL8125B，当前`ethtool -c/-l`返回不支持，sysfs只有rx-0/tx-0；
ring为256。不要自动安装另一套网卡驱动或修改寄存器来追求合并参数。
本机EEE是enabled-inactive，板端EEE已disabled；目前没有活跃EEE，关闭它只是控制变量候选，不能预称有收益。
板端stmmac支持读取coalescing，RX327us、TX1000us/25frames，ring512。
**TX合并主要是完成中断/回收，不等于报文先被扣住1000us再发送**。
本地驱动拒绝TX usecs/frames同时为0，且RX传0不会走更新RIWT分支；不能用一键清零脚本。
先查支持范围/读回，再逐项试TX100us+frames1等合法候选；板端RX不是视频主要接收方向，优先级较低。

host出站qdisc是fq_codel，作用于主机发送，不能用删它来优化主要的视频入站流。
板端NET_SCHED未开；启用qdisc/pacing属于单独镜像实验，只有TX积压/IDR突发证据后才做。
保留checksum offload，GRO/GSO逐项核实，不修改TCP拥塞算法来优化UDP。
MTU仍1500、报文仍1200；增大报文需同时改接收buffer和重组stride/协议兼容，收益低于先消除显示阻塞。

本机只有软件时间戳、无PHC，板端支持PHC0。暂不能假设有双端硬件PTP精度，
先测host内核RX→应用、socket积压及软件RTT，而不输出未经同步的单向网络微秒数。

## 4. CPU/GPU 与进一步裁剪

host实际为`amd-pstate-epp`、governor=powersave、EPP=balance_performance，GPU=auto。
powersave在这个驱动下不是“锁死最低频率”。先单项比较EPP=performance，再另测governor，
读回频率/功耗；GPU独立比较已有合法performance档。保留热管理，不把桌面永久锁满频。
[AMD pstate说明](https://docs.kernel.org/admin-guide/pm/amd-pstate.html)

板端没有cpufreq policy或DMC devfreq节点，只有VENC/NPU；available_governors没有performance。
现有S02写performance忽略错误，不能说CPU/DDR已锁满频。先查合法时钟/OPP归属及VENC忙时频率，
再只在支持的模式中试验；不直接超频或假设新增governor会自动创建CPU/DMC节点。

继续裁剪仍有价值，但先按独立组做：

1. 无线/BT内核、外置wifi模块、rkwifi/wpa/蓝牙服务联动关闭；去掉nginx/fcgi、无用输入服务及OEM演示自启。
   替换RkLunch时保留udev、CIF/ISP参数、五个MPP/Rockit核心模块和已验证的静态SSH网络，去掉重复eth DHCP客户端。
2. NPU→DRM/VOP/DSI/FB/backlight，重建外置模块并检查依赖；用户态libdrm继续保留。
3. 不用的NFS/CIFS/FUSE/NTFS、USB-host、触摸屏/HID等按依赖关掉；USB gadget/ADB先保留，最后再全裁USB。
4. Buildroot移除板端Python/FFmpeg/GStreamer/桌面/无线utilities/开发与sample文件；
   保留librockit、MPP、libasound、libdrm、SSH及诊断工具。源码/staging保留开发库，目标镜像删除。
5. 最后单测移除TELEOP_QSPINLOCK_DIAG、KALLSYMS_ALL等诊断开销；保留可回退诊断镜像。
   关闭未运行驱动或DWARF不会自动省去一帧等待，不将镜像大小改善混写成视频延迟改善。

不重新“启用PREEMPT/HZ1000”：已经开了。PREEMPT_RT、NO_HZ_FULL/isolcpus、盲目缩ring或全线程最高FIFO都后置。

## 5. 编译选项：本轮已做隔离兼容性验证

sender/viewer目前CMake Release都是O3，kernel是O2，Buildroot主要Os。
再次加O3不会改变前两者。推荐未来增加独立CMake开关：IPO、host native arch、Aura CPU tune，默认关闭，
用CheckIPOSupported明确拒绝不支持的请求；不要用wrapper掩盖工具链能力。
[CMake IPO检查](https://cmake.org/cmake/help/latest/module/CheckIPOSupported.html)

| 对象/候选 | 本轮验证 | 后续判断 |
|---|---|---|
| viewer GCC16 O3+IPO+march=native | native构建、2个CTest、2个真实VAAPI测试通过，覆盖125帧 | 可进入逐项软件A/B；native在本CPU检测为znver5，保留portable版本 |
| viewer Clang23 O3+ThinLTO+native+LLD | 同样通过；有2处现有整数转换warning | 先修portability warning；不因一次兼容性测试就宣称更快 |
| sender GCC12 O3+mtune=cortex-a53 | Distrobox交叉编译、AArch64 ELF/RUNPATH/库检查通过 | 尚未上板运行，也未部署；下一轮与原程序单项A/B |
| sender标准CMake IPO | **失败**，CheckIPOSupported=NO | SDK GCC配置enable-lto/disable-plugin，无liblto_plugin.so，仅.a；不能直接开IPO |
| kernel GCC O2+A53 tune | 先前编译器接受，但未完整镜像A/B | 保留kernel语义/通用寄存器/errata flags，单独构建内核和模块 |
| kernel Clang O2→ThinLTO | vendor Kconfig有Clang LTO路线，Distrobox当前缺完整LLVM工具 | 后置；需要时请用户装依赖，先Clang无LTO及所有外置模块兼容，再ThinLTO |
| Buildroot Os→O2 | 配置可选，尚未A/B | 只对运行热点或独立镜像比较；先裁后台，勿全局O3/LTO一把梭 |

sender的旧小型直接LTO链接成功，不能代表含teleop-common静态库的标准CMake IPO可以工作。
若要推进sender LTO，应评估完整插件工具链或Clang+现有glibc/sysroot，验证C++ABI/动态库后再使用；
本轮没有伪造插件/包装编译器，也没有下载包。

PGO放在代码路径稳定、剖析确认CPU热点之后：用单独instrumented构建采集多场景/IDR/双路数据，
退出后取回profile重建，再测正式二进制。训练成本不计入性能结果。
LTO/PGO只覆盖自有源码，不会重优化现成FFmpeg、libplacebo、librockit动态库或硬件编解码器。
O2也应对照O3，避免假设更激进一定更快；不启用Ofast/fast-math、不直接关闭当前依赖的C++exceptions。
[GCC优化说明](https://gcc.gnu.org/onlinedocs/gcc-12.4.0/gcc/Optimize-Options.html)，
[AArch64 tune选项](https://gcc.gnu.org/onlinedocs/gcc-12.4.0/gcc/AArch64-Options.html)，
[Linux LLVM构建](https://docs.kernel.org/6.1/kbuild/llvm.html)。

本轮host两种候选共250个VAAPI测试帧，属于localhost兼容性检查，**不是速度对比**。
Clang warning为protocol.cpp:32的uint16转换、performance.cpp:19的signedness。
探针在ignored `build/probes/`，日志在 `logs/next-latency-options/20261013/`；现用二进制哈希未变。
[运行时状态、探针结果与哈希](baselines/2026-10-13-next-latency-options.json)。

## 6. 后期挑战：真正的slice流水

mode1仍须得到完整AU后发送，之后网络和decode可能还能与编码重叠。
mode3目前每slice都给bFrameEnd=true、公开MPI不能可靠指明真正最后slice，旧PTS分组会恢复额外一帧等待。
需要先从KMPP真实EOI元数据贯通到用户态，再设计slice/帧结束协议；接收可先传输期间重组整AU，
然后评估FFmpeg/VAAPI对分片提交的真实约束。不能直接把slice标为完整AU，或凭固定pack数猜帧尾。
双路wrap曾出现串扰/绿色条纹，继续wrap0；没有双路wrap正确性证据前不恢复。
这条路线可能改变流水重叠，值得研究，但成本和验证风险高于viewer线程拆分。

## 7. 建议下一轮的交付边界

第一轮实现“分段显示计时 + RX时间戳/overflow + 显示与收包解耦”，不动内核/编译选项。
保留可切回同步路径的参数。先用native合成双路流人为注入20/100ms present延迟，验证收包继续、
解码参考链正确、有界队列不累积、latest显示替换生效、无输入也能关闭窗口。
再做实机每路120帧的相同软件对照、双路画面与生命周期回归；长测仍需要用户明确安排散热。

第二轮单独引入非阻塞recvmmsg、buffer池/少复制，再逐项测busy-poll/EPP/IRQ；
每个变量保存原值并读回/恢复，测试阶段不让netconsole verbose、满速iperf或逐包printf混入结果。
第三轮只变一个编译选项，对同一程序版本分别测GCC O2/O3、native/tune、IPO，再决定是否做PGO。
第四轮另建无线/显示/NPU/Buildroot裁剪镜像，以同一程序做A→B→A；需要刷写由用户操作。
只有正确性、队列年龄和阶段尾延迟实测支持的选项才进入默认配置，最终screen-to-screen另测。
