# 完整AU立即发送与解码：双路短测通过

已将[帧边界探索](frame-boundary-wait-exploration-2026-10-13.md)中的两项应用优化接入正式程序，
交叉编译并部署到板端`/userdata/teleop-sender`，本机`build/host/bin/host-viewer`和
`build/host-metrics/bin/host-viewer`均在CachyOS原生编译。
内核、SDK库、镜像、DTS、曝光及VBLANK没有改变，不需要重新烧写。
板端KMPP仍为已验证的IRQ修复模块；没有自动重启或烧写。

## 最终行为与兼容方式

- sender新增`--output-mode frame|slice`，默认frame。frame使用SDK split mode1，
  确认GetStream返回一个非空pack、bFrameEnd=true后即发送，不再等下一PTS。
  不符合该模式的pack形状会报错并按原流程释放资源，不能悄悄发送半帧。
- slice使用mode3，继续按相同PTS聚合，收到下一PTS后才确认上一AU完整。
  mode3每个slice的bFrameEnd都可能为true，所以这一兼容路径仍不信任该字段。
- 两种路径发出的payload均为完整AU，设置协议flags bit1（值2，kCompleteAccessUnit）。
  bit0（值1）仍为IDR。协议版本仍1、头长仍40字节；旧viewer忽略新标记仍可解析，
  但保留自己的parser等待。通用UdpSender没有自动标记任意payload，USB及旧slice发送端仍可走旧路径。
- receiver要求同一帧的全部分片flags一致；缺片或标记冲突的数据不提交解码。
  收齐且有完整AU标记时直接调用H264VaapiDecoder；无标记时保留Annex-B parser。
  每路契约独立，支持一路新AU、一路旧字节流；同一路中途改变契约会明确要求重启viewer，
  避免把parser里未完成的数据与直接输入混合。文件回放仍使用字节流parser。
- mode1的SPS/PPS+IDR pack有时被MPI标成PPS，所以sender也扫描Annex-B NAL header，
  保留真实IDR标记。实机验证两路第一帧均标成IDR、每路120帧中8个IDR。
- 新的`sender.ready_to_send`计时覆盖最后pack取得后到准备发送的处理。
  保留`sender.next_pts_wait`作为历史对照别名；frame模式下这个名字不再表示等待下一PTS。
- viewer新增`--force-stream-parser`，仅用于强制恢复旧lookahead的对照。
  启动脚本默认`OUTPUT_MODE=frame`，设为slice可对照原SDK输出模式。

SYS所有权、进程锁、双路初始化/绑定顺序、已完成worker继续排空另一侧完成前的数据，
以及异常路径的stream release均沿用已有实现。frame模式有限N帧不再需要第N+1帧来确认；
单路fake MPI测试验证发送3帧仅取3次，slice两包/帧则仍取7次完成3帧。

## 实机A/B结果

两路AR0234 1280×720约99.23fps，30Mbps、GOP15、slice参数120、wrap0。
各正常用例每路120帧，未使用allow-long；离散before/after快照温度44.19–46.41°C，
启动门槛70°C。未启用netconsole，watchdog/IRQ保持默认；USB读取内核日志。
优化前二进制先保存到ignored的`artifacts/frame-boundary-before-20261013/`并重新短测。

同一镜像上四组对照，以下均值单位ms：

| 路径 | sender最后pack→准备发送 c0/c1 | host ready→提交 c0/c1 | host ready→VAAPI AVFrame c0/c1 |
| --- | --- | --- | --- |
| 原sender + 原viewer | 7.857 / 7.876 | 9.994 / 10.027 | 10.090 / 10.154 |
| 新mode1 + 新viewer | 0.108 / 0.134 | 0.0017 / 0.0026 | 0.113 / 0.144 |
| 新mode3 + 新viewer | 7.922 / 7.966 | 0.0022 / 0.0031 | 0.091 / 0.129 |
| 新mode1 + force-stream-parser | 0.108 / 0.201 | 9.995 / 10.026 | 10.120 / 10.173 |

优化路径sender最后pack→准备发送P95为0.127/0.193ms，host ready→提交P95为
0.0031/0.0038ms；host ready→VAAPI对象P95为0.141/0.178ms。
两项应用确认/提交等待之和相较原路径约少17.7ms，但这是进程局部阶段的对照，
没有减去两台机器的时钟，也没有测screen-to-screen或完整编码输入→输出延迟。
mode1改变了slice输出组合，不能把第一pack时间定义的改变直接当成全部编码加速。
取得VAAPI AVFrame对象也不意味着GPU或屏幕扫描已经完成。

三组新路径和原路径均完整接收并硬解240帧，malformed、duplicates、dropped均0。
mode3对照保留sender等待、强制parser对照恢复主机等待，分别验证两项变化来源。

## 回归与显示验证

- Ubuntu22.04 Distrobox交叉编译sender、目标动态库/RUNPATH检查通过。
- Distrobox native测试目录`build/distrobox-boundary-unit`：protocol、performance、
  MPI生命周期3个CTest通过，启动脚本8项测试通过。
  初次使用的旧aura-lifecycle目录含AArch64缓存，无法在x86容器执行；已换独立目录重建并通过。
- protocol覆盖完整AU标记保留、乱序、flags冲突拒收、正确重传、丢片后下一个完整AU恢复。
- native CachyOS VAAPI测试包含5种双路场景：指标关闭、旧输入、新输入、新旧混合、强制parser。
  另一个丢片场景使用全IDR独立帧，验证一个残帧不进解码器、后5个完整帧硬解成功。
  共125个本机硬解帧，两个Python测试通过。它不保证任意P帧丢失后立即无损恢复。
- 板端完整生命周期10个用例通过：正常双/单路、三处故障注入、TERM/INT、进程锁、恢复双路。
  注入失败退出1是预期值；所有trace用例均成功WaitFreeMB和SYS_Exit。
- 双路Vulkan/SDL3实显240帧通过。用native UDP转发器查看wire flags后转给viewer，
  所有分片有完整AU标记，两路首帧有IDR标记；两路各120个VAAPI硬件帧，presenter共240次。
  此转发仅用于线协议检查，显示阶段数据不能直接与直连headless组作网络延迟比较。
- 本轮实机完整接收/硬解合计2160帧，最终无活跃sender或MPP sessions；pool used与常驻基线一致，
  除常驻BufGrp和BufGrpCfg各1个外均0。USB日志没有hardlock、softlock、BUG或WARNING调用栈。
  旧CIF vblank提示仍存在，没有为此修改用户已验证的sensor时序。

显示短测有一次约95.3ms的首次present调用尖峰；两路present_call P95约1.95/1.90ms，
呈现初始化、shader预热以及串行收包/显示阻塞值得后续独立排查。
本轮不把首次尖峰隐藏在均值中，也未做长时间稳定性或最终屏幕延迟验收。

## 当前使用方式

先在CachyOS启动新版viewer（窗口上下两路布局沿用现有实现）：

```sh
./build/host/bin/host-viewer \
  --dual-stream --frames 0 --timeout-ms 3600000 \
  --window-width 1080 --window-height 960 --borderless
```

再在板端启动已部署脚本，默认就走本轮优化：

```sh
/userdata/run-ar0234-stereo
```

诊断对照才使用`OUTPUT_MODE=slice /userdata/run-ar0234-stereo`或viewer的
`--force-stream-parser`。这两种设置分别恢复一个帧边界等待，日常运行不需要添加。
二进制、原始日志、录像及构建目录均ignore，没有覆盖用户的tools/configure-host-rj45。
新sender SHA256为`06583eb5ab52c824f2d8fab1cbea24839fb81aef547060e9d84eb900456a33bd`。

[完整阶段数据、用例结果和日志指纹](baselines/2026-10-13-frame-boundary-optimization.json)。
后续先保留此应用基线，再对显示预热/队列或内核裁剪做独立A/B；不再把本轮旧parser等待算成内核问题。
