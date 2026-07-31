# RK3576 双 IMX586 模拟触发与 frame_id 绑定测试

## 1. 这次实现了什么

仍然只使用一个正式可执行程序 `camera_aiq_test`。新增的软件链路是：

```text
2/4 Hz 软件定时器                    现有双路 V4L2 采集线程
       |                                      |
 TriggerEvent                         VIDIOC_DQBUF
 trigger_id + 两种时钟               camera_id + sequence + V4L2 时间戳
       |                                      |
       +-------------- 绑定队列 --------------+
                          |
            trigger_id -> cam0/cam1 frame_id
                          |
              状态查询 + CSV 记录
```

模拟器只产生 `TriggerEvent`，不会改变 GPIO，也不会在 `FSYNC_CAM` 上产生电平。
因此它用于验证应用层数据链路，不能代替 MCU、示波器或物理 XVS 验收。

## 2. “V4L2 取帧和队列绑定”是什么

V4L2 的采集缓冲队列和这里的触发绑定队列是两件事：

1. 采集线程先用 `VIDIOC_QBUF` 把空缓冲交给驱动。
2. 驱动/ISP 填完一帧后，应用通过 `poll` 和 `VIDIOC_DQBUF` 取回缓冲。
3. `v4l2_buffer.sequence` 是该路帧号，`timestamp` 是该帧的 V4L2 单调时间戳。
4. 程序只复制这几个元数据到绑定队列，然后立即继续原有保存、UVC、网络输出流程。
5. 缓冲处理完后再用 `VIDIOC_QBUF` 还给驱动循环使用。

原有 `capture_backend.cpp` 已经完成 1、2、5，这次没有重写采集程序。新增的是第 4
步的轻量元数据回调。它不复制 18 MB 的 NV12 图像，不会为绑定再开一套 V4L2
节点，也不会改变现有采集、保存和输出的所有权。

绑定规则当前为 FIFO：每个有效 trigger 依次等待 cam0 和 cam1 的下一帧；两路帧
到达顺序可以不同，两路都到齐后生成一条完整记录。IMX586 Slave 模式第一次 XVS
用于 pre-shutter 时，可以在重置命令中忽略第一个触发。

## 3. 新增文件与接口

- `trigger_simulator.h/.cpp`：进程内 2 Hz/4 Hz 软件触发源。
- `trigger_frame_binder.h/.cpp`：触发队列、双路 frame_id 配对、统计和 CSV。
- `capture_backend.h/.cpp`：每次 `DQBUF` 后上报帧元数据。
- `camera_aiq_test.cpp`：命令入口和硬件无关自检。
- `Makefile.camera_aiq`：仍只构建 `camera_aiq_test`。

## 4. 第一层测试：完全不依赖相机和 MCU

板卡上执行：

```sh
cd /root/camera_uart
./camera_aiq_test --sync-bind-self-test
```

预期唯一关键结果：

```text
SYNC_BIND_SELF_TEST_OK source=SIM triggers=2 pairs=2 frame_delta_ns=200000
```

自检真实启动 2 Hz 定时器，并给每个模拟触发注入一对测试帧元数据。cam1 先到、
cam0 后到，用于覆盖双路线程到达顺序不同的情况。通过条件包括：

- 模拟触发数为 2；
- 完整双路帧对为 2；
- cam0/cam1 各绑定 2 帧；
- 最后一条 trigger_id 为 2；
- 最后一对帧号为 cam0=202、cam1=102；
- 两路模拟时间差为 200000 ns。

该结果证明软件算法和线程链路，不证明相机真的因 XVS 曝光。

## 5. 第二层测试：接现有 V4L2 取帧

启动同一个程序：

```sh
cd /root/camera_uart
./camera_aiq_test
```

输入：

```text
sync-bind-log /tmp/xvs_sim_binding.csv
sync-bind-reset 0
stream-start all
sync-sim-start 2 10
wait 6000
sync-sim-status
capture-status all
sync-bind-status
sync-bind-last
sync-sim-stop
stream-stop all
quit
```

`sync-sim-start 2 10` 表示每 500 ms 产生一个应用层模拟事件，共 10 个。输出明确
带有 `physical_xvs=0 source=SIM`，防止把它误认为硬件脉冲。

如果两颗 IMX586 当前固定为 XVS Slave 且没有外部脉冲，合理结果是：

```text
emitted_triggers=10
frames=0 或不增长
complete_pairs=0
pending=10
```

这是正确结果：软件定时器不能让 Slave 传感器出帧。若相机处于自由运行，或测试时
手工短接产生了帧，`complete_pairs` 会增长；这只说明 V4L2 帧元数据进入了绑定队列。
因为软件事件与手工短接没有共同硬件时基，仍不能宣称“一次 XVS 对应一对帧”。

CSV 字段为：

```text
trigger_id,source,trigger_monotonic_ns,trigger_realtime_ns,
cam0_sequence,cam0_flags,cam0_timestamp_ns,cam0_delay_ns,
cam1_sequence,cam1_flags,cam1_timestamp_ns,cam1_delay_ns,frame_delta_ns
```

`frame_delta_ns` 是两路 V4L2 buffer 时间戳差，不是两颗传感器曝光起点差。

## 6. 后期接真实 MCU 如何改

绑定层不需要修改。模拟器和 MCU 适配器都调用同一个入口：

```text
trigger_frame_binder_on_trigger(trigger_id, monotonic_ns,
                                realtime_ns, source)
```

接入 MCU 时保留 V4L2 回调和绑定层，只做以下变化：

1. 停止调用 `sync-sim-start`。
2. MCU 每产生一个真实 XVS 下降沿，就通过 UART 主动上报一个事件。
3. RK3576 的 UART 接收线程校验 CRC、检查 trigger_id 连续性，并调用同一绑定入口，
   `source` 改为 `MCU`。
4. `START/STOP/STATUS` 的命令应答和异步触发事件必须在 UART 接收线程中分流，
   不能让异步事件破坏现有 ACK 等待。

建议 MCU 异步事件正文：

```text
EVT,XVS,<trigger_id>,<mcu_tick_us>,<pps_id>
```

完整线路仍使用现有 `$...*CRC16\r\n` 封装。`mcu_tick_us` 必须在实际 XVS 边沿的
定时器中断/捕获单元中锁存，不能在 UART 发送时才读取。若需要 UTC，MCU 还要锁存
最近 PPS 的 `pps_id`，并将 NMEA UTC 与该 PPS 对齐。

MCU 时钟与 RK3576 `CLOCK_MONOTONIC` 不是同一时钟域。正式版本必须通过 PPS/时钟
映射把 `mcu_tick_us` 转换到统一时间轴；仅使用 UART 报文到达时间只能做顺序绑定，
不能计算真实 XVS 到 SOF 的精确延迟。

## 7. 真实 MCU 到位后的测试顺序

IMX586 的第一个 XVS 用作 pre-shutter 时：

```text
sync-bind-log /root/camera_test/mcu_xvs_binding.csv
sync-bind-reset 1
stream-start all
sync-start 4
wait 10000
sync-bind-status
sync-bind-last
sync-stop
stream-stop all
```

然后执行 1000 次定量触发，验收：

- MCU 实际 trigger_id 连续，无重复和缺号；
- cam0/cam1 各新增 1000 帧；
- `complete_pairs=1000`、`pending=0`；
- 两路 `sequence_drops=0`；
- 停止 XVS 后两路帧数不再增长；
- 示波器确认 FSYNC/PPS_OUT/cam0 XVS/cam1 XVS 的电压、脉宽、周期和边沿差；
- 统一时钟后统计两路 SOF 差的最大值、平均值和 P99。

只有这一层完成，阶段 5 的真实硬件同步与精度验收才算完成。

## 8. 本次已验证与尚未验证

已验证：

- RK3576 目标程序交叉编译通过；
- 2 Hz 软件触发线程工作；
- trigger_id 与双路 frame_id FIFO 绑定工作；
- cam0/cam1 逆序到达仍能形成正确帧对；
- 状态统计和 CSV 输出工作。

尚未验证：

- 新程序在当前板卡上的运行结果（测试时 `10.100.2.67:2222` SSH 无法建立连接）；
- 模拟事件与当前板卡两路真实 V4L2 帧的联调；
- MCU 异步逐脉冲报文接收；
- 物理 XVS、PPS/NMEA、统一时钟和同步精度。
