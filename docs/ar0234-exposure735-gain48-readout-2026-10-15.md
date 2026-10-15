# 735 / 48 / 512 参数下的双路读出实测

2026-10-15，用户认可的曝光/增益组合：`EXPOSURE=735 ANALOGUE_GAIN=48 DIGITAL_GAIN=512`。
实机为四通道 #18 内核，每行6.8µs，735行曝光=4.998ms；VBLANK762不变。
**两轮双路短测，读出仍约4.9ms，未因该曝光/增益组合拉长到另一档。**

| 路 | 读出均值 | P95 | 最大观测值 | 帧周期均值 |
|---|---:|---:|---:|---:|
| camera0 | 4.876ms | 4.937ms | 4.939ms | 10.074ms |
| camera1 | 4.894ms | 4.915ms | 4.924ms | 10.078ms |

每轮每路120编码帧，两轮总480帧；原生CachyOS viewer全部收到并完成VAAPI硬解，
malformed/duplicates/dropped均为0，没有解码警告。USB活标记后的日志无新的内核WARNING/BUG/锁死或CSI错误，
结束温度约47°C、boot ID不变，无sender/采样器遗留。摄像头参数读回确认735/48/512，
测试后恢复到启动前的同一组参数，没有更改launcher默认值。

每路40个有效读出快照，约50ms采样，按双方sender-ready门限排除初始化阶段，并按帧号去重。
P95/max是这些短测快照的统计，不是全部480帧或长时间运行的尾延迟保证。
这是CIF帧开始→帧结束的驱动计时，包含IRQ时间偏差，不是曝光结束→ISP完成或screen-to-screen。
本次只核对出流、硬解和时序，没有重新评价画面亮度或运动清晰度。
[精简数据](baselines/2026-10-15-ar0234-exposure735-gain48.json)。

在原生CachyOS、连接USB/ADB后复测（输出目录需不存在）：

```sh
./tools/test-ar0234-readout --camera dual --frames 120 --expect-lanes 4 \
  --exposure-us 4998 4998 --analogue-gain 48 --digital-gain 512 \
  --output artifacts/readout-735-48-512-new
```

基础软件测试工具之前将模拟增益写死32；本轮为它和readout工具增加显式
`--analogue-gain`、`--digital-gain`，保留原默认32/512，minimal/launcher两种路径都传递。
readout工具按实际sensor控件范围校验，在每轮结束读回曝光与两种增益；
同时解析CIF帧周期，按未补零的微秒余数处理`rate`，防止十进制误读。

Ubuntu22.04 Distrobox：readout工具10项测试、codec工具4项回归通过。
真实摄像头/UDP/VAAPI测试在原生CachyOS执行，没有重新编译或刷写内核。
原始日志在ignored `artifacts/ar0234-fastreadout-20261015/exposure735-gain48/`。
