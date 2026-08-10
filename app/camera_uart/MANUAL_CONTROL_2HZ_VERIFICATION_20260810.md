# RK3576 双 IMX586 2 Hz 手动参数确认延迟修复与实测

## 1. 问题

旧程序在 2 Hz 采集时连续执行 `exposure`、`gain`、`iso`，可能约 3.5 秒后
`manual_settings_verified` 才变为 1，并且必须再次执行 `status` 才能刷新状态。

## 2. 原因

1. 三条设置命令都会通过 `rk_aiq_user_api2_ae_setExpSwAttr()` 提交一份完整的
   “曝光时间+模拟增益”目标。连续执行三条命令等于连续提交三次，每次都会使前一
   次确认失效，并从最后一条命令重新计时。
2. `gain` 和 `iso` 是同一个模拟增益目标的两种输入方式。当前程序以基础 ISO 50
   换算：`gain_x1000 = iso * 1000 / 50`，同一目标不应同时设置两者。
3. 旧程序只在执行 `status` 时查询 RKAIQ，因此 3.5 秒中还混入了人工等待和下一次
   查询的时间，不能代表传感器固定延迟。
4. RKAIQ 设置接口返回成功只表示目标被接受。目标还需要经过 AIQ 参数队列、
   传感器寄存器更新、下一次 XVS/曝光、ISP 帧流水线，之后查询结果才会匹配。
   2 Hz 每帧约 500 ms，所以低帧率下按毫秒看起来会明显变慢。

## 3. 修复

- 新增 `manual CAMERA_ID EXPOSURE_US GAIN_X1000`，一次提交曝光和增益。
- 帧率切换后恢复手动参数也改为一次组合提交。
- 每路相机保存独立的 request id、请求时 V4L2 sequence 和请求时间。
- 每收到一个真实的新 V4L2 帧，只唤醒该路后台确认线程一次。
- 首次回读匹配后自动输出 `CAMERA_VERIFY_COMPLETE`，不再依赖人工重复执行
  `status`。
- `status` 增加确认帧号、经过帧数和确认耗时。

连续快速下发多个目标时，后一个 request id 会取代前一个目标，只验收最终目标。
这是正确语义；2 Hz 下两个帧之间没有新图像，无法为每个中间目标生成已生效帧。

## 4. 板端实测

环境：RK3576、双 IMX586、两路实际采集均为 2.000 fps，无 sequence drop。

| 测试 | cam0 | cam1 | 结果 |
| --- | --- | --- | --- |
| `manual` 组合设置 | 5000 us、3 倍；seq 140 -> 145，5 帧，2291 ms | 20000 us、6 倍；seq 140 -> 145，5 帧，2312 ms | 通过 |
| 单独设置 | 曝光 8000 us；seq 324 -> 329，5 帧，2128 ms | 增益 4 倍；seq 324 -> 329，5 帧，2149 ms | 通过 |
| ISO 设置 | ISO 100；seq 595 -> 600，5 帧，2174 ms | ISO 400；seq 595 -> 600，5 帧，2194 ms | 通过 |

最终回读：

- cam0：7996 us、2.000 倍、估算 ISO 100；
- cam1：19996 us、8.000 倍、估算 ISO 400；
- 两路 `manual_settings_verified=1`、`manual_settings_pending=0`；
- 两路采集 `fps_x1000=2000`、`sequence_drops=0`。

新程序 SHA256：

```text
7d7562e222618842d7b42252f791bd93a8d680b4d3be625e9ce8ed090ba8ebf9
```

板端旧程序已备份为：

```text
/root/camera_uart/camera_aiq_test.before_manual_verify_fix
```

## 5. 正确测试方法

```text
stream-start all
wait 4000
capture-status all

manual 0 5000 3000
manual 1 20000 6000
wait 4000
status all
```

验收 `CAMERA_VERIFY_COMPLETE` 和以下字段：

```text
manual_settings_verified=1
manual_settings_pending=0
manual_verified_sequence_valid=1
manual_verification_frames=<实际帧数>
manual_verification_latency_ms=<实际耗时>
```

只修改一个参数时可以使用 `exposure`、`gain` 或 `iso`。`gain` 与 `iso` 二选一。

## 6. 结论和边界

旧的约 3.5 秒并不是单一硬件故障。重复提交和只能由 `status` 刷新的程序问题已
修复；实测剩余延迟稳定为 5 帧、约 2.1 至 2.3 秒，属于当前 2 Hz 模式下可观察到
的 RKAIQ/传感器/ISP 流水线延迟。

`manual_verified_sequence` 表示“程序在该采集帧到来后首次查询到匹配值”，不是
传感器曝光寄存器的精确生效起点。若需求是测量 XVS 沿到真实曝光起点的微秒级
精度，仍需传感器曝光指示信号或示波器/逻辑分析仪，不能只用应用层回读替代。
