# AR0234 1ms 曝光预览

已通过SSH将两路exposure设为74行，读回确认；按当前13.6µs行时间为1.0064ms。
曝光不是固定10ms，735行是之前为看清暗区设定的值。
模拟增益32、数字增益512、分辨率与sensor时序均未改。

每次直接NV12抓图跳过10帧，再保存1帧；两种曝光、两个相机共44帧。
原图位于ignored `captures/ar0234/exposure-1ms-20261014/`：

| 相机 | 原约10ms | 新约1ms | Y均值，10ms→1ms |
|---|---|---|---|
| 0 | camera0-10ms.png | camera0-1ms.png | 83.17→28.86 |
| 1 | camera1-10ms.png | camera1-1ms.png | 126.89→22.93 |

1ms图片明显更暗，保留原始亮度，没有自动提亮。已请求在本机面板打开camera0-1ms.png。
当前镜头画面主要是室内天花板/灯具；需更好的照明或另做增益对照评估可用画质。
本轮未运行MPP或viewer实时长测，也未测新的光学延迟。

直接`v4l2-ctl`停流触发了3条`dma_buf_put` WARNING，栈含`rkisp_stop_streaming`/
`destroy_buf_queue`，首次发生在原735行曝光的camera0停流。
全部四张图采集完成后，日志检查发现告警。截图与控制读回均成功，但严格kernel检查失败，控制器自动恢复曝光735；
随后只执行控制修改，重新设为74并读回两路，未继续启动采集。
不把这次抓图称为无告警的稳定性验证；告警暂未定位或修复。
原USB日志保存在上述图片目录，详细汇总见
[记录](baselines/2026-10-14-exposure-1ms.json)。

仅运行时修改，不改SDK、驱动默认值或launcher默认值。
再次从launcher启动时须明确指定曝光，否则默认会回写735：

```sh
# 在板上执行；用户手动运行，默认持续发送
EXPOSURE=74 /userdata/run-ar0234-stereo
# 有限帧预览
EXPOSURE=74 /userdata/run-ar0234-stereo --frames 120
```

重启会恢复驱动默认控制值。原10ms预览参数可通过`EXPOSURE=735`选择。
原控制值、模式/节点及读取结果已保存；图片/原NV12/临时脚本均不提交Git。
