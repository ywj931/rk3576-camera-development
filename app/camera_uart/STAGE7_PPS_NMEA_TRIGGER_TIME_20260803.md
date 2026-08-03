# RK3576 双 IMX586 PPS/NMEA/Trigger 时间同步实现

日期：2026-08-03

## 1. 当前结论

阶段 7 已按照需求文档“三、同步与时间戳方向”的推荐逻辑完成软件链路：

```text
MCU 捕获 PPS 边沿和硬件计数器
        |
        +-- EVT,PPS / EVT,RMC(NMEA) --> RK3576 UTC 映射
        |
MCU 产生 XVS，并在同一计数器上记录 Trigger
        |
        +-- EVT,XVS --> trigger_id + trigger_time_utc_ns
                              |
                    双路 V4L2 frame sequence 绑定
                              |
                    CSV + JPEG EXIF/UserComment
```

主机模拟测试和 RK3576 交叉编译已经通过。由于当前没有真实 MCU、GNSS PPS/NMEA
和示波器数据，这只能证明软件接口与绑定流程可运行，不能代替真实硬件同步精度验收。

## 2. 与推荐同步逻辑的对应关系

| 需求 | 当前实现 | 状态 |
| --- | --- | --- |
| 捕获 PPS 上升沿和本地高精度计数器 | MCU 发送 `EVT,PPS`，携带 `pps_id` 和 `timer_tick` | 软件接口完成，待真实 MCU |
| PPS 后接收 GPRMC/NMEA | 支持 `EVT,RMC` 和原始 `EVT,NMEA`，解析 GPRMC/GNRMC | 完成 |
| 下一个 PPS 使用上一个 GPRMC 时间加 1 秒 | `TimeSyncService` 在下一次 PPS 建立 UTC 基准 | 完成 |
| 从 PPS 使用本地计数器连续计时 | 通过 `--sync-timer-hz` 将 `timer_tick` 换算为 UTC ns | 完成 |
| 记录相机 Trigger 时刻 | MCU 发送同一计数器域的 `EVT,XVS` | 软件接口完成，待真实 MCU 边沿 |
| Trigger 与两路 frame_id 绑定 | `TriggerFrameBinder` 绑定 cam0/cam1 的 V4L2 sequence | 完成 |
| 保存 camera_id/frame_id/曝光/增益/时间戳 | CSV、JPEG EXIF 和 UserComment | 完成 |

当前曝光值来自保存照片时查询到的 RKAIQ 最新状态，不是传感器对该 frame_id
锁存的逐帧曝光值，因此明确标记为 `RKAIQ_LATEST_NOT_FRAME_BOUND`。正式同步精度
验收前，还需要获取逐帧曝光寄存器/ISP metadata，并标定固定帧延迟和 sensor offset。

## 3. 时间映射原理

PPS、XVS 必须由 MCU 的同一个单调硬件计数器计时。建立 UTC 基准后：

```text
event_utc_ns = pps_utc_ns
             + (event_timer_tick - pps_timer_tick) * 1,000,000,000
               / timer_frequency_hz
```

程序默认 MCU 计数器频率为 1 MHz，可通过以下参数修改：

```bash
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

RK3576 收到 UART 字节时的 `CLOCK_MONOTONIC` 时间只用于诊断串口延迟，不能当作
真实 XVS 边沿时刻。相关输出会显示 `monotonic_is_uart_arrival=1`。

## 4. MCU 异步事件协议

MCU 主动向 RK3576 上报：

```text
$EVT,PPS,<pps_id>,<timer_tick>*<CRC16>\r\n
$EVT,RMC,<pps_id>,<utc_sec>,<valid>*<CRC16>\r\n
$EVT,NMEA,<pps_id>,<$GNRMC...*HH>*<CRC16>\r\n
$EVT,XVS,<trigger_id>,<pps_id>,<timer_tick>*<CRC16>\r\n
```

建议时序：

1. PPS N 上升沿到达，MCU 锁存计数器并发送 `EVT,PPS`。
2. MCU 收到该秒 GPRMC/GNRMC 后发送 `EVT,NMEA` 或已解析的 `EVT,RMC`。
3. PPS N+1 到达后，RK3576 用上一条有效 RMC 的 UTC 秒加 1 建立锁定。
4. MCU 产生每次 XVS 时锁存同一计数器并发送 `EVT,XVS`。
5. RK3576 把 trigger UTC、cam0 frame_id、cam1 frame_id 写入绑定记录。

完整 CRC 和字段约束见 `MCU_XVS_UART_PROTOCOL.md`。

## 5. 板端运行与状态检查

```bash
cd /root/camera_uart
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

程序内执行：

```text
time-sync-reset
sync-bind-reset 1
stream-start all
time-sync-status
sync-controller-status
```

在开始正式保存前，`time-sync-status` 应满足：

```text
state=UTC_LOCKED
utc_valid=1
pps_count 持续增长
rmc_valid_count 持续增长
```

短时缺少 NMEA 时允许进入 `HOLDOVER`；超过配置的 holdover 周期后 UTC 失效，
程序不得把该 Trigger 标为有效绝对时间。

## 6. 已完成的软件测试

在 PC 上执行：

```bash
make -f Makefile.camera_aiq check-time-sync
make -f Makefile.camera_aiq check-stage7-host
make -f Makefile.camera_aiq check-xvs-uart
make -f Makefile.camera_aiq check-control-uart-host
make -f Makefile.camera_aiq check-stage6-host
```

测试覆盖：

- GPRMC/GNRMC 解析、校验和、日期换算和无效报文拒绝。
- “PPS 后 RMC、下一 PPS 加 1 秒”锁定逻辑。
- MCU 计数器到 UTC 纳秒的换算和 holdover 失锁。
- PPS/RMC/NMEA/XVS 与命令 ACK/NACK 在同一 UART 接收线程中的分流。
- trigger_id 与双路 frame sequence 的绑定。
- JPEG APP1/EXIF 的 UTC、曝光、ISO 和 UserComment 字段。

RK3576 可执行程序使用 SDK 交叉工具链构建：

```bash
make -f Makefile.camera_aiq aarch64
```

## 7. 接入真实 MCU 后的验收流程

建议先固定 2 Hz，不做 2/4 Hz 动态切换：

```text
time-sync-reset
sync-bind-reset 1
stream-start all
time-sync-status
photo-offset 0 <cam0_offset_ns>
photo-offset 1 <cam1_offset_ns>
photo-start all /tmp/stage7
sync-start 2
wait 10000
sync-status
sync-bind-status
time-sync-status
sync-stop
photo-stop all
stream-stop all
```

验收时检查：

- UTC 锁定后才开始保存。
- 每个 trigger_id 恰好对应 cam0、cam1 各一个 frame_id。
- 两路帧数差为 0，`sequence_drops=0`，没有重复绑定和未绑定项。
- 2 Hz 连续 1000 次触发时，两路各新增 1000 帧。
- 停止 XVS 后两路帧数不再增长。
- CSV 和 JPEG EXIF 中 UTC、frame_id、camera_id、曝光、ISO/gain 一致。
- 内核无 rkcif、rkisp、MIPI 和 IMX586 错误。

示波器需同时观察 PPS、FSYNC_CAM、cam0 XVS 和 cam1 XVS，记录电压、周期、脉宽、
毛刺和两路到达偏差。软件的 V4L2 buffer 时间戳只能做辅助诊断，不能证明曝光起点
的硬件同步误差。

## 8. 尚未完成的硬件闭环

以下项目必须接入真实硬件后完成，当前不能标记为最终验收通过：

1. MCU 捕获真实 PPS、解析 GNSS NMEA，并产生稳定的 1.8 V XVS。
2. MCU 的 PPS 与 XVS 使用同一硬件计数器，验证计数器频率和回绕处理。
3. 示波器验证 PPS、FSYNC_CAM 和两颗 IMX586 XVS 的边沿及时序误差。
4. 2 Hz/4 Hz 各进行 1000 次以上连续触发和丢帧测试。
5. 标定 `trigger_time -> exposure_start/exposure_center` 的 sensor 固定延迟。
6. 将曝光/增益改为与具体 frame_id 绑定的传感器或 ISP metadata。

软件侧的一 UART 多路复用已经完成：`--uart /dev/ttyS9` 只打开一次设备，并统一
分流 `$CAM`、`$XVS` 的 ACK/NACK 和 `$EVT`。伪终端测试已覆盖控制命令执行期间
嵌套发起 `$XVS STATUS` 的场景，确认接收线程不会死锁。真实 MCU、电平、PPS、XVS
和 1000 次物理触发仍属于上述硬件闭环验收，软件模拟通过不能替代它们。
