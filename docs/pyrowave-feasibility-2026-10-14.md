# PyroWave 与 Aura 双目遥操链路的可行性

结论：**值得借鉴它以带宽换计算和丢包恢复的设计，但当前 RV1126B 不能直接用 MPP
硬件执行 PyroWave。** 本机有合适的 Vulkan GPU，板端没有对应的通用 shader 硬件。
建议先测现有 H.264 的真实硬件完成时间，再做硬件全 I 帧/MJPEG 对照；CPU/NEON
版 PyroWave 是一个新的编码器移植项目，不能承诺比现有硬件编码更快。

本轮仅研究和只读检查：没有修改 SDK、sender/viewer、镜像或运行策略，没有烧录，
没有启动摄像头。板端 SSH banner exchange 超时，ADB 无设备，因此没有本轮板端
运行证据，也没有在本机编译/跑 PyroWave；不能把能力检查写成编解码性能测试。

后续同日已恢复 SSH/USB 并完成当前 H.264 链路实测：编码硬件约2.51ms，
双路AU提交到VAAPI实际完成约1.83ms。新的收益预算见
[编解码完成时间与PyroWave预算](codec-completion-and-pyrowave-budget-2026-10-14.md)；
本页保留初次调研时的检查范围，不将后续实测混入上游PyroWave benchmark。

## 已核实的上游信息

截至 2026-10-14，Valve 的 Remote Play 官方说明支持手动设置 **100–500 Mbit/s**，
约 12.5–62.5 MB/s；500 MB/s 则是 4 Gbit/s，不是同一数量级。
自动质量模式按帧大小工作，帧率增加会增加带宽；500 Mbit/s 不是算法固定需求，
也不是对所有分辨率/帧率的最高可能码率。Valve 明确提醒网络与显示也会决定最终延迟。
[Valve 官方公告](https://steamcommunity.com/groups/homestream/discussions/0/564794422009744473)

开源实现：[Themaister/pyrowave](https://github.com/Themaister/pyrowave)，MIT 许可。
本轮锁定 `a0d2e71e98b85f6a23f62407df21a39e9745b875`，提交时间
2026-10-12 23:25:59 UTC（北京时间 10-13）。独立 C API/共享库依赖 Granite 的部分组件，
上游脚本锁定 Granite `fb178c8080d163419e8d20f10715c61c53c1ec9b`；
README 仍提醒 API/ABI 在发展中。
[构建说明](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/README.md)，
[依赖版本](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/checkout_granite.sh)，
[许可证](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/LICENSE)。

它按帧独立编码，采用 CDF 9/7 小波和五级分解，舍弃运动预测及复杂熵编码，
便于 GPU 并行处理。码流 v1 在 2026-10-03 冻结，独立系数块为 32×32；
README 的 64×64 简述与规范不同，移植应以锁定版本的实现/规范为准。
缺失块可按零系数重建，低频块丢失比高频块严重；不能把所有丢包都理解为轻微模糊。
[v1 码流规范](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/bitstream/bitstream.md)

作者公布过 RX 9070 XT/RADV 上 1080p 编码约 0.08–0.13 ms、解码低于 0.1 ms 的例子。
这是特定 GPU/画面的阶段结果，不是 Aura 的数据，也不是 camera-to-screen 时间。
作者画质比较中的 H.264/HEVC/AV1 还使用了全 I 帧、严格 CBR、最快模式等条件，
与我们当前 GOP15 的低延迟 H.264 不等价。
[作者技术文章](https://themaister.net/blog/2025/06/16/i-designed-my-own-ridiculously-fast-game-streaming-video-codec-pyrowave/)

源码 benchmark 用 GPU timestamp 计编码/解码；编码后的 bitstream readback 另计。
相机 DMA-BUF 的获取/导入、CPU 打包/发送、网络、呈现和屏幕扫描均应另测。
[计时实现](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/bench.cpp)

## 为什么不能直接移植到 MPP

PyroWave 使用 Vulkan compute，而非 Vulkan Video 的标准视频编解码接口。
编码需要 subgroup 运算/尺寸控制、shaderInt16、8-bit storage；FP16 shader 为可选。
即使调用 `pyrowave_encoder_encode_cpu()`，也只是把 CPU 中的像素交给 GPU 编码，
并非纯 CPU 后端。当前解码还提供面向部分移动 GPU 的 fragment shader 路径，
仍然需要可编程 GPU。
[C API 与特性要求](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/pyrowave.h)，
[编码设备检查](https://github.com/Themaister/pyrowave/blob/a0d2e71e98b85f6a23f62407df21a39e9745b875/pyrowave_encoder.cpp)。

RV1126B 的公开框图列出 Cortex-A53、NPU、VEPU/VDPU 和 RGA，没有通用 3D/compute GPU。
RGA 的缩放、格式转换、旋转不提供任意 shader；VEPU 执行特定编码算法。
添加 MPP codec enum 或重编驱动不会让它获得 DWT、量化、码率分析和自定义打包电路。
这是依据公开硬件/软件接口作出的可行性判断，不是本轮板端运行验证。
[Rockchip 官方简版规格与框图](https://www.rock-chips.com/uploads/pdf/2026.2.11/192/RV1126B%20brief%20datasheet%20V1.0%2020241219.pdf)，
[MPP 上游](https://github.com/rockchip-linux/mpp)。

本地 SDK 的 `media/mpp/out/include/rockchip/rk_type.h` 及 Rockit `rk_common.h`
只有既有标准 codec 类型；`media/mpp/mpp/mpp/hal/rkenc/jpege/` 存在 JPEG HAL。
枚举包含某 codec 不能独立证明 RV1126B 对它的全部模式、双路吞吐和运行时支持。
Rockchip 的完整 datasheet 描述 JPEG 编码/解码和 2D RGA，可将 MJPEG 列入实测候选。
[Rockchip datasheet v1.4，第 11–12 页，镜像副本](https://rockchip.fr/RV1126B%20datasheet%20V1.4.pdf)

可能的移植路线：

| 路线 | 评估 |
|---|---|
| PyroWave → Aura MPP/VEPU | 无对应硬件/API，不能直接实现硬件加速 |
| CPU/NEON 编码 → 本机 Vulkan 解码 | 算法可重写；须完整实现 DWT、量化/码率分配、打包和互通测试，先做吞吐/延迟探针 |
| NPU 承担部分小波计算 | 研究方向；即使部分卷积可表达，码率分析、位操作、CPU/NPU同步与拷贝仍需解决，3 TOPS 不等于可跑本算法 |
| 有兼容 Vulkan GPU 的采集平台 → 本机 GPU | 最接近上游设计；必须验证实际特性、DMA-BUF 导入和相机路径，不能仅按芯片写了 Vulkan 判断 |
| Aura H.264 全 I 帧 / 硬件 MJPEG | 最少改动的“以带宽换独立帧”对照，速度和双路能力尚需实測 |

本机只读 `vulkaninfo` 检查：Radeon 890M/RADV，设备 API 1.4.354；
shaderInt16、storageBuffer8BitAccess、shaderFloat16、subgroupSizeControl 均为 true，
subgroup 32–64，编码要求的 subgroup operation 均有。
**基础特性符合，尚未验证 PyroWave API、外部图像互操作或实际速度。**
后续应先运行上游 `pyrowave-device-validation --roundtrip`，再测真实尺寸与显示整合。

CPU 路线要处理双路约 184.32 M 个亮度像素/秒；NV12 原图输入约 276.48 MB/s，
还不含五级变换、中间缓冲和多遍访存。四核 A53 是否有余量只能由板端探针回答。
通过 MPP 分配/复用 DMA-BUF 可以帮助数据管理，但编码工作仍由 CPU 执行。
CPU Vulkan 驱动也不等于获得 GPU 加速。

## 双路 720p/100fps 的带宽与延迟预算

以下均为十进制单位。PyroWave 的 1/1.5/2 bpp 是**待测试的预算假设**，
不是实机码率或本项目画质要求；bpp 指整帧压缩数据位数/亮度像素数。
以目标 100fps 算，板端此前观测约 99.23fps。计算和环境记录见
[JSON](baselines/2026-10-14-pyrowave-feasibility.json)。

| 双路 1280×720@100 | 视频有效码率 Mbit/s | 两路每帧周期数据的千兆理想串行化 ms | 沿用现有包长的线上估计 ms |
|---|---:|---:|---:|
| 现有 H.264，30 Mbit/s/路的平均预算 | 60.00 | 0.60 | 0.66 |
| PyroWave 假设 1 bpp | 184.32 | 1.84 | 2.01 |
| PyroWave 假设 1.5 bpp | 276.48 | 2.76 | 3.02 |
| PyroWave 假设 2 bpp | 368.64 | 3.69 | 4.02 |

线上估计按本项目 IPv4 UDP：1200 B datagram，其中 40 B 项目头、1160 B 视频；
额外包含 UDP/IP、Ethernet/FCS/preamble/IFG，没有 VLAN、FEC、重传或其他流量。
PyroWave 原生 packet 边界与分片策略必须另设计，这个表只是沿用现有传输的预算。
H.264 的实际 I/P 帧大小有波动，表中平均预算不能当成每帧固定值。

1.5 bpp 下，每相机每帧 172800 B，沿用当前分片约 149 包/帧，
双路约 29800 包/秒；当前 30 Mbit/s/路平均预算约 6600 包/秒。
因此板端 send/网络栈/内存压力也会增加，不能只关注带宽是否小于 1 Gbit/s。

表中时间是**一条共享网口将两路同一帧周期的数据发完所需链路占用**；
不是每个相机必然新增的端到端延迟。具体延迟取决于两路相位、交错发送和计算/网络重叠。
若两路各 500 Mbit/s，视频自身已经吃满千兆，加协议后不可持续；单合流 500 Mbit/s
在 100fps 下每周期仅有效数据也占 5ms。更快编码会被更长传输部分抵消。

无压缩同样有约束：单路 720p/100fps Y8 为 737.28 Mbit/s；双路 Y8 为 1.475 Gbit/s，
双路 NV12 为 2.212 Gbit/s，现有千兆无法承载。若允许降低空间分辨率，
双路 640×360@100 Y8 为 368.64 Mbit/s，可用来做无 codec 对照，但不能拿它与 720p
画质等价比较。Y8 会舍弃色度；不能在未确认应用接受灰度时默认采用。

## 是否会比现在更低延迟

有兼容 GPU 的采集端可能获益；当前 Aura 上重写成 CPU 编码后，收益未知。
现有 H.264 使用完整 AU 即时提交，已经移除旧的下一帧边界等待；
不能假设 P 帧天然增加一整帧缓存，也不能把旧路径的收益再次计算给 PyroWave。

最近本机短测，AU 完成→FFmpeg 返回帧均值约 0.16–0.19ms；
此时 VAAPI 返回的对象**不保证 GPU 解码已完成**。板端 GetStream wait 约 9.5ms
包含等待下个相机帧/硬件出流，**不是单独的编码耗时**。
目前没有足够证据计算“换 codec 可再省 X ms”，更不能从 0.1ms benchmark
推导我们的 screen-to-screen 会变成 1ms。
[现有软件对照及限制](scheduling-power-ab-2026-10-13.md)，
[slice 与真正帧尾的约束](next-latency-optimization-options-2026-10-13.md#6-后期挑战真正的slice流水)。

PyroWave 的完整图像变换和全帧码率分析仍然需要图像输入准备好；
它不是摄像头曝光/行读出/ISP 延迟的替代品。帧内独立最确定的价值是更自由地舍弃旧帧、
降低丢包后参考链的恢复成本，而我们当前只允许替换解码后的帧。

## 建议的验证顺序

1. 保持当前 H.264 基线。补测可关联的采集/ISP 输出→编码完成，以及 GPU 真正完成
   解码→渲染依赖满足；硬件 fence/trace 要单独做 observer-effect 对照。
   不把两个主机未同步的 MONOTONIC 时间相减，不用 GetStream 阻塞时长冒充编码耗时。
2. 单变量测 H.264 `GOP=1`，先维持相同码率，再做可接受画质的较高码率组；
   比较实际 AU 大小/突发、编码完成、CPU、队列、GPU 完成和丢包恢复。
   全 I 帧不自动保证更快，不能预先设成默认值。
3. 验证 Aura 硬件 MJPEG 的单路、双路支持和 native 主机解码；质量档/峰值码率
   不一定能硬封顶，必须测最差场景。现有 wire flags/parser 约定 H.264 AU，
   需要显式 codec 协商/版本扩展及拒绝错配，不能把 JPEG 字节送进 H.264 解码器。
4. 原始 Y8/较低分辨率路线作为独立诊断，证明省下的 codec 时间是否足以抵消网络。
   保持传感器时序，若另加 VPSS 缩放，明确测它的额外开销。
5. 若仍值得研究 PyroWave：先 native 锁定版本构建，跑官方互通测试和 720p GPU
   encode/decode/readback 分段探针；再在 Distrobox 交叉编译 CPU/NEON 小波编码探针，
   上板只跑短样本。不先重写整个 sender。通过真实图像的画质、p95/p99、双路吞吐、
   无界积压排除后，再扩展 codec backend 和 packet 协议。

后续实机沿用每路 120 帧、USB 独立日志、70℃启动阈值；不自动长测、重启或烧录。
在相同曝光/帧率/分辨率、相同像素数据与明确画质标准下比较；低熵灰度、文字/机械细节、
运动和传感器噪声均应覆盖。PyroWave 的丢包部分重建应另测低频保护和过期帧策略，
不能把 PyroWave 的可丢帧策略直接套到 H.264 参考链。

调研源码、完整 vulkaninfo、SSH 超时记录和计算中间数据存于 ignored
`artifacts/pyrowave-research-20261014/`，只提交说明和汇总，未安装任何系统软件包。
