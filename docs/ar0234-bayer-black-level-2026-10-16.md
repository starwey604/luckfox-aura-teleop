# AR0234：Bayer 与黑电平实机核对

2026-10-16，用户要求先检查下方 camera1。结论：**本次实际 CFA 与 GRBG 一致，
没有发现驱动/ISP 相位错配；ISP 白平衡增益、黑电平扣除和 CCM 均未开启。**
当前增益下遮光 RAW10 的黑场均值约42DN，各通道最大均值差约1.16DN，
绿色通道没有更高的黑场偏置。偏绿优先处理白平衡，黑电平和色彩矩阵应按顺序完善。
没有修改或固化 ISP 参数，没有修改 SDK/内核，没有刷写、重启或恢复持续出流。

## 条件与安全边界

- 内核 `6.1.141 #19 SMP PREEMPT`；boot ID `98c5eb51-6403-48ac-8f89-d96c4c544f2f`。
- camera1：`m01_b_ar0234 3-0010`、`/dev/v4l-subdev6`、CIF `/dev/video12`、
  ISP `/dev/video32`、params `/dev/video38`。
- 1280×720、四通道，VBLANK506，曝光735、模拟增益48、数字增益512。
  曝光约4.998ms，实际帧周期预算8.3368ms。
- 每次启动前检查没有其他摄像头程序、温度低于70°C，并验证USB `/dev/kmsg` 独立通道的实时marker。
- 一次单路MPI编码120帧，用于运行状态/寄存器快照；七次独立CIF RAW采样，每次12帧，共84帧。
  RAW采样没有启动ISP/MPP，避免混用摄像头资源。
- 所有测试boot ID相同，各自marker之后没有新的CSI错误、内核WARN/lockup；温度约45–49°C。
- RAW采样临时使用16-bit little-endian低位对齐，实际stride2560、每帧1843200字节；
  完成后恢复原compact模式、stride1792、sizeimage1290240。没有改动sensor控件。
- 初始RAW序号0..3之后有vendor启动表产生的间隙；分析只使用最后4帧（通常seq31..34），
  不把启动阶段样本计入黑电平或颜色统计。

## Bayer 证据

驱动、CIF、ISP均报告 `MEDIA_BUS_FMT_SGRBG10_1X10`。
水平/垂直翻转都是0；sensor裁剪起点(328,248)均为偶数；CIF/ISP偏移(0,0)。
ISP运行时寄存器 `ISP_CTRL1@0x0404=0x00000008`，按当前BSP的Bayer字段为GRBG。

用户让平板打开浏览器色彩靶。先拍九宫格，随后将原色由192降低至64码值，避免饱和，
依次采集 `#400000` / `#004000` / `#000040` / `#404040`。
固定RAW ROI `(x=530,y=450,w=40,h=40)`，四帧均值如下，均没有饱和像素：

| 屏幕目标 | 偶行偶列 Gr | 偶行奇列 R | 奇行偶列 B | 奇行奇列 Gb |
|---|---:|---:|---:|---:|
| 红64 | 65.24 | **112.86** | 52.54 | 63.84 |
| 绿64 | **128.47** | 72.32 | 63.99 | **127.45** |
| 蓝64 | 58.22 | 53.21 | **101.24** | 58.39 |
| 灰64 | 154.93 | 141.46 | 123.97 | 154.16 |

这组实际光学响应对应：

```text
G R G R ...
B G B G ...
```

不需要更改Bayer排列。仅看白屏不能得到这个结论；RGB参照与RAW位置响应是关键证据。
平板有光谱、刷新、色彩管理与环境反光影响；灰64有绿色响应偏高，但不能据此生成
适用于室内灯光的最终白平衡系数。屏幕黑色那组出现了浏览器UI，已剔除校准用途；
屏幕黑色本来也不等同于传感器遮光黑场。

## 运行中的 ISP 实际状态

`RKISP_CMD_GET_PARAMS_V35`在当前镜像返回EINVAL，因为
`CONFIG_VIDEO_ROCKCHIP_ISP_VERSION_V35_DBG`未开启。ABI大小核验通过；
内核源码中的该getter明确提供返回EINVAL的stub，所以没有把失败当成“参数都是0”。
最终使用一次短时MPI出流期间的 `/proc/rkisp-vir1` 和现有procfs寄存器dump核对，
只启用dump模式1，没有打开寄存器写入模式；结束设回mode0。

| 模块 | 实机状态 |
|---|---|
| DEBAYER | ON，bypass0，CTRL=0x111 |
| AWBGAIN | OFF；gain0 G/RB均为0x01000100（四通道寄存器值256） |
| BLS | OFF，CTRL=0；A/B/C/D固定扣除寄存器均为0 |
| OB | OFF；offset0、offset1=0、predgain=0 |
| CCM | OFF |
| GAMMA_OUT | OFF |
| CSM | FULL，当前系数对应BT.601转换 |
| 空域/时域降噪等 | 这次快照中多数OFF；此轮没有调整 |

这些是本次实际运行快照，不能泛指所有历史镜像或不同应用的ISP状态。
相同通道gain寄存器值并不等于正确白平衡；AWBGAIN本身也是关闭的。

## 遮光黑场

用户确认已完全遮住下方camera1镜头后，按同一曝光/增益再次采12帧；分析最后4帧、
每通道921600个样本。数值单位都是**RAW10 DN（0..1023）**。

| 通道 | 均值 | 中位数 | 全样本标准差 | P1–P99 | 时间噪声RMS |
|---|---:|---:|---:|---|---:|
| R | 42.82 | 44 | 11.55 | 17–70 | 10.17 |
| Gr | 41.66 | 43 | 11.57 | 15–69 | 10.20 |
| Gb | 41.66 | 43 | 11.39 | 15–67 | 10.06 |
| B | 42.54 | 44 | 11.33 | 16–68 | 10.05 |

无饱和像素；约0.16–0.19%样本已经为0，不能通过简单扣常数恢复这些样本。
时间噪声RMS来自4帧每个像素的样本方差再做空间平均，仅是短样本估计。

黑场通道差很小，且绿色均值略低，不能用它解释明显偏绿。
不过约42DN的残余黑场偏置应在加白平衡前合理扣除，否则通道增益会把偏置一起放大。
不要直接把42写进ISP寄存器：RAW10采样单位、ISP内部位深/缩放和BLS模块位置需要先核实。
该结果只适用于这次曝光/增益与温度，不能直接假设全增益范围或另一颗模组完全相同。

## 复用工具与后续

`tools/capture-cif-raw.c`仅负责已有1280×720 GRBG节点的独立CIF采集：
共享sender的排他锁、限制1..120帧、拒绝覆盖文件、校验RAW低位对齐，结束恢复CIF格式。
它不管理USB日志和温度，使用前仍必须建立独立日志、确认温度及无其他采集程序。

在Ubuntu22.04 Distrobox中编译（无需添加SDK源码或重新构建镜像）：

```sh
distrobox enter aura-sdk -- bash -lc '
  cd /home/ww/codings/luckfox-aura-teleop
  mkdir -p build/colour-probes
  ../aura-sdk/tools/linux/toolchain/aarch64-rockchip1240-linux-gnu/bin/aarch64-rockchip1240-linux-gnu-gcc \
    -O2 -Wall -Wextra -Wpedantic -Wconversion -Werror \
    -idirafter ../aura-sdk/sysdrv/source/kernel/include/uapi \
    tools/capture-cif-raw.c -o build/colour-probes/capture-cif-raw
'
```

使用`-idirafter`保留工具链已导出的标准Linux头文件优先，只从当前SDK补充Rockchip UAPI；
直接把内核未导出的`include/uapi`放在`-I`最前会缺少`linux/compiler.h`，不要创建假头文件。
上传至板端后，单次用法：

```sh
timeout -s INT -k 3 8 /tmp/teleop-capture-cif-raw \
  /dev/video12 /tmp/<本次唯一名字>.raw 12 > /tmp/<本次唯一名字>.jsonl
```

拉回后在本机运行`tools/analyse-cif-raw`，依赖本机已有NumPy；可选预览需要已有Pillow。
Distrobox只用来交叉编译，不需要给它安装图像分析依赖。

```sh
./tools/analyse-cif-raw captures/example.raw --metadata captures/example.jsonl \
  --skip 8 --preview captures/example-preview.png
```

分析器验证stride、帧完整性、低位对齐标记与RAW10范围，按GRBG分别统计；
预览仅做2×2块采样和显示gamma，没有执行ISP白平衡/CCM，不能当成精确调色后的图像。
本轮还验证了padding剔除、CFA位置映射、坏格式/截断/位深拒绝，
以及板端超出帧数、错误节点、已有输出文件的拒绝路径。交叉编译启用上述所有告警并视为错误。

完整原始数据、USB日志、ISP快照及短片保留在忽略目录：
`artifacts/ar0234-colour-20261016/`；精简数值见
`docs/baselines/2026-10-16-ar0234-colour.json`。

后续顺序：核实BLS数值尺度 → 在当前增益下验证扣除及暗部裁剪 → 实际照明下白纸/灰卡
手动白平衡 → 必要时色卡调CCM/gamma → 验证双路帧率/队列/延迟。无需为了这一步修改Bayer或sensor时序。
