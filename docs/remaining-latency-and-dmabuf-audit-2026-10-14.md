# 30–40ms 光学延迟中的剩余阶段与 DMA-BUF 检查

结论：**主要 DMA-BUF 路径已接通，剩余延迟不能直接归因于缺少零拷贝。**
优先检查曝光/读出和实际显示呈现，再调整 H.264 参数。当前4.34ms的codec阶段估算
并没有覆盖整个采集与显示过程；30–40ms是用户本次报告的光学结果，本轮没有重新测它。

本轮仅做源码、板端控制值和本机桌面状态的只读检查，未启动摄像头测试、调整曝光、
改桌面设置或程序，没有修改SDK或烧录。
[状态与算术记录](baselines/2026-10-14-remaining-latency-audit.json)，
[实际codec完成时间](codec-completion-and-pyrowave-budget-2026-10-14.md)。

## 已确认的采集端时间尺度

两颗AR0234当前控制值都是 exposure735、hblank1168、vblank21、pixel_rate180MHz。
1280像素的HTS为2448，驱动以行数定义曝光：

```text
一行       = 2448 / 180000000 = 13.6µs
735行曝光  = 9.996ms
帧周期     = (720+21) × 13.6µs = 10.0776ms，约99.23fps
720行预算  = 9.792ms
```

后续缩短该预算的可行性与四通道 A/B 设计见
[720 行读出时序探索（2026-10-15）](ar0234-readout-time-options-2026-10-15.md)。

最后一个数只是sensor模式的有效行输出时间预算，不是测到的完整ISP读出延迟。
全局快门同时积分并不意味着所有像素会在同一瞬间传完MIPI；曝光、sensor存储/读出、
CIF/ISP、共享资源调度和等待下一采样帧都还会影响图像年龄。
曝光与读出可以有流水重叠，不能把这些预算不加分析地相加。

约10ms曝光会跨越144Hz屏幕的约6.94ms刷新间隔，时间码可能包含多个显示状态。
若保持读出节奏不变，信息有效时间的积分中心随短曝光向后移动，是潜在的数毫秒优化点；
不能简单承诺10→2ms曝光必然让光学延迟下降8ms。

建议后续做735行（10ms）→147行（约2ms）→74行（约1ms）的单变量对照，
固定帧率、增益与其他设置；另做照明/增益组以维持可接受画质，不开启会改变帧率的自动曝光。
本轮未写任何控制值。`run-ar0234-stereo`支持环境变量`EXPOSURE`，默认735；
现有software-baseline的单路控制脚本也会回写735，曝光A/B必须将参数贯通，
不能手工改完后又用默认launcher覆盖它。

板端`/proc/rkisp-vir0/1`读回1280×720@99fps、Inp0x100；SDK对应`INP_CIF`。
这些停流后的状态不能证明运行时没有额外ISP/VENC排队或读回，也不能将停流时100MHz
ISP时钟当成采集中实际工作频率。下一步应以源帧标识关联SOF/EOF、ISP输出、
VENC开始/完成和GetStream，区分读出时间与完整一帧缓冲。
ISP帧timestamp可能来自上游时间，必须核实它表示SOF、EOF还是其他参考点。

## DMA-BUF 覆盖到了哪里

| 环节 | 当前实现 | 判断 |
|---|---|---|
| Aura VI→VENC | `VI_V4L2_MEMORY_TYPE_DMABUF`，`RK_MPI_SYS_Bind(VI,VENC)` | 应用没有取出NV12再复制给编码器 |
| VENC码流→UDP | MB映射、完整AU复制进vector，sendmmsg用header/payload两段iovec | 还有压缩字节拷贝；已避免逐包拼接payload |
| host UDP→FFmpeg | 重组到连续buffer，av_new_packet后复制 | 可做buffer池/AVBufferRef接管，需padding和生命周期 |
| VAAPI→Vulkan | libplacebo将VAAPI直接映射DRM PRIME，导入DMA-BUF纹理 | 无应用层解码像素下载再上传 |
| Vulkan→屏幕 | 渲染RGB swapchain，再交给Wayland/KWin | 尚未证明直接scanout，合成和显示同步独立于decode零拷贝 |

libplacebo7.360.1在`pl_map_avframe_derived()`使用`MAP_READ|MAP_DIRECT`，
随后`pl_map_avframe_drm()`用`PL_HANDLE_DMA_BUF`与format modifier/stride导入纹理。
硬件路径失败会报错，没有静默换成软件像素上传。
这确认了应用共享路径；GPU驱动内部是否做detile/blit仍需专门测，不能宣称没有任何物理拷贝。
[libplacebo实现](https://github.com/haasn/libplacebo/blob/v7.360.1/src/include/libplacebo/utils/libav_internal.h)

本机FFmpeg n9.0.2的VAAPI→DRM导出，在READ模式下先`vaSyncSurface()`，
再`vaExportSurfaceHandle()`。**零像素拷贝仍须等待生产者完成**。
上一轮真实窗口的map_call约0.67/0.70ms，包含部分解码等待，不能与1.83ms的完整解码时间
重复计入总预算；显示batch_call约2.22ms也包含它们，不是额外2.22ms纯显示硬件开销。
[FFmpeg实现](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavutil/hwcontext_vaapi.c)，
[DMA-BUF与同步](https://docs.kernel.org/driver-api/dma-buf.html)。

DMA-BUF用于同一台机器中设备共享内存，不会让开发板的buffer跨Ethernet直接变成本机的buffer。
网络仍然传压缩字节。剩余压缩AU拷贝值得优化CPU开销，但目前没有证据表明它们导致了
一二十毫秒。sender到首次发包约0.1ms量级，不能把“不是全链路零拷贝”当作巨大瓶颈。

后续可测已完成surface的导出/导入时间，再决定缓存Vulkan纹理。
当前每次render都map/unmap两路最新帧，即使某路没有更新也会再导入。
缓存必须绑定AVFrame/真实surface及buffer世代和GPU完成依赖，不能只按可复用的fd整数缓存。
不能去掉同步来获得更好数字；可研究把CPU等待改成正确GPU fence依赖，以增加流水重叠，
而不是让Vulkan读取未完成图像。

## 显示端的额外时间

只读`kscreen-doctor -o`确认：副屏DP-1为1920×1080@144.02Hz，rotation2、scale1.35，
逻辑区域800×1423；主屏为100Hz。未更改配置。
旋转和小数缩放是渲染/合成变量，不足以单独证明增加了多少延迟或无法直接scanout。
测试窗口应完全位于目标副屏，记录实际像素大小与逻辑坐标。

viewer请求MAILBOX、swapchain_depth1。libplacebo不支持所请求模式时会退回FIFO；
depth1只限制GPU在途render工作，不限制实际swapchain image数，也不限制KWin内部队列。
当前没有记录实际选择的present mode、buffer被最终呈现或丢弃的时间。
[libplacebo swapchain](https://github.com/haasn/libplacebo/blob/v7.360.1/src/vulkan/swapchain.c)

144Hz有每周期约6.94ms的显示时机粒度；100fps视频和144Hz扫描之间有变化的相位，
最新帧也可能等下一次显示时机。错过deadline可能再多一个周期，但本轮没有证实
KWin固定多排了一帧。GPU render返回、swap返回都不等于像素已经出光。
面板内部处理和液晶响应时间目前未知，不能按广告标称响应时间扣除。

建议接入可关联每次提交的presentation feedback或受支持的Vulkan呈现计时接口，
记录output、refresh、presented/discarded、HW时间/完成标志、zero_copy等信息；
该反馈可由compositor近似，不代替最后的光学测试。
Vulkan WSI可能负责wl_surface提交，需检查SDL/Mesa与反馈的关联方式，不能在另一连接
随意发feedback请求后声称测到了同一次vkQueuePresent。
[Wayland呈现协议](https://github.com/wayland-mirror/wayland-protocols/blob/main/stable/presentation-time/presentation-time.xml)

旋转后的逻辑上下不一定是面板物理扫描上下。旧方案“上下码必然位于不同扫描线”的表述
已修正；用本机同源图案直接复制到两位置校准，不假定偏差就是半屏3.47ms。
源时间码采样于绘制前，固定源显示年龄在同屏时间码差中可部分相消，
也不能把源窗口present等待机械地再加一次。

## 缓冲数量与真正延迟不是同一回事

VI buf_count3/depth0、VENC stream buffers5、默认编码队列上限4、解码后latest mailbox1。
最近headless实测编码队列peak1，AU queue wait约18–21µs；真实窗口mailbox等待也通常
远小于1ms。不能把3个VI buffer直接乘成30ms，或认为5个VENC buffer必定积压50ms。
这些常是所有权/硬件流水/输出容量；是否延迟要看帧年龄和实际队列深度。
viewer的condition_variable wait_for5ms可被通知立即唤醒，不是固定每帧sleep5ms。

缩buffer可能减少最坏积压，也可能导致饥饿、丢包或驱动异常；先用序号/时间戳
证明有额外旧帧，再单变量测试。H.264参考帧仍不能像已解码帧一样任意丢弃。

## 推荐优化顺序

1. **采集曝光A/B及SOF/EOF→出流观测**：735/147/74行，保持现有时序；
   验证亮度/噪声/时间码可读性，定位是否有完整一帧ISP/VENC排队。
2. **实际显示呈现与100%缩放对照**：保留144Hz与当前布局，记录实际present mode，
   原1.35缩放与100%分开测。将来做fullscreen对照时需处理同屏测试图布局，
   不把fullscreen自动等同于direct scanout。源码中的MAILBOX参数尚不是可选CLI。
3. **H.264参数A/B**：当前High profile；分别研究Baseline/CAVLC、8×8 transform开关、
   deblock、QP/CBR/FIXQP、GOP15/1和slice大小。SDK公开了SetH264Entropy、SetH264Trans、
   SetRcParam等接口，先读回实际值和确认支持，再单变量测硬件cycles、GPU完成、
   AU大小/IDR突发与画质。硬件不一定因较简单工具而更快；码率增加会带来传输成本。
4. **按实测热点减少重复导入、AU拷贝和分配**：保持解码/收包分离，测收益后再做
   buffer池或正确的surface缓存；同步优化以增加重叠为目标。
5. **最后挑战真实行/slice流水**：目前mode1仍等完整AU，开启slice参数不等于边编码边显示。
   wrap双路曾串扰，必须先保证采集/编码buffer及可靠EOI；沿用wrap0直到有双路正确性证据。
   这条路线可能减少阶段串行，不能预先承诺节省完整一帧。

前三项更可能触及毫秒或帧周期量级；本轮没有足够证据给出30–40ms的逐项精确拆账。
后续软件层测量仍沿用每路120帧、70℃启动阈值、独立USB日志和observer-effect对照。
不自行重启或烧录；最后再用光学实验验证整体效果。
