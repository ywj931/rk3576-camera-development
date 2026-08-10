# RK3576 双 IMX586 本地相机控制与采集保存

这个程序是阶段 4 相机控制、输出和阶段 5 同步联调的统一入口。当前仍然只生成一个可执行程序 `camera_aiq_test`，一个进程统一持有：

- cam0/cam1 两路独立 RKAIQ context；
- cam0/cam1 两路 V4L2 NV12M 采集线程；
- 两路独立的异步保存线程和状态；
- cam0/cam1 两路独立 4000x3000@2/4fps MJPEG UVC 输出；
- cam0/cam1 两路独立 HTTP MJPEG 网络输出，实际更新率跟随各自 2/4 Hz 相机源；
- 曝光、模拟增益、帧率、开始/停止出流、开始/停止保存和状态查询命令；
- 一条 `/dev/ttyS9` 115200、8N1 全双工链路，统一承载相机控制、MCU XVS
  命令/应答、PPS/GPRMC/Trigger 事件；
- PPS/GPRMC UTC 对时，以及 trigger 与双路 frame_id 绑定。

UVC 将 camera0、camera1 分别输出到 `uvc.0`、`uvc.1`，依赖 USB gadget 按
`DUAL_UVC_4000X3000_TEST_20260731.md` 配置。
`sync-status` 的 V4L2 时间戳比较和软件模拟触发都属于诊断工具，不代表两颗
IMX586 已经在同一触发沿曝光。模拟与 MCU 迁移说明见
`TRIGGER_FRAME_BINDING_SIM_TEST.md`。

## 文件和资源归属

- `camera_backend.h/.cpp`：双路 RKAIQ 初始化、独立曝光/增益，以及 IMX586 XVS 2/4 Hz 独立分频和回读。
- `capture_backend.h/.cpp`：双路 V4L2 出流、帧统计、异步 NV12 保存和保存状态。
- `camera_uvc_backend.h/.cpp`：cam0/cam1 独立帧队列、MPP MJPEG 编码和 UVC 状态。
- `camera_net_backend.h/.cpp`：cam0/cam1 独立最新帧队列、10 fps 主动节流、
  MPP JPEG 编码、HTTP MJPEG 多客户端输出和服务状态。
- `trigger_frame_binder.h/.cpp`：trigger_id 与 cam0/cam1 frame_id 队列绑定。
- `trigger_simulator.h/.cpp`：不产生物理 XVS 的 2 Hz/4 Hz 应用层模拟触发源。
- `time_sync_service.h/.cpp`：解析 RMC，将下一 PPS 锁定到 UTC，并把 MCU
  计数器时间换算为 UTC 纳秒。
- `xvs_uart_controller.h/.cpp`：唯一的 UART 所有者，统一分流 `$CAM`、XVS
  命令应答和 PPS/RMC/XVS 异步事件。
- `camera_control_uart.h/.cpp`：UART 帧解析、命令映射和 ACK/NACK 返回。
- `camera_aiq_test.cpp`：统一的本地命令和 UART 命令入口。
- `Makefile.camera_aiq`：板卡原生构建和 SDK 交叉构建，只输出 `camera_aiq_test`。
- `camera-http.service`：双摄采集和 HTTP 输出的 systemd 常驻服务。
- `camera-uvc.service`：双摄采集、双 UVC 输出和 UART 控制的常驻服务。

新程序默认打开 `/dev/video22` 和 `/dev/video31`。执行 `stream-start` 后，不要再运行旧的 `v4l2-ctl` 或 `v4l2_frame_tap` 占用同一节点。

## 默认映射

| camera_id | IQ 目录 | ISP 参数事件节点 | 采集节点 |
| --- | --- | --- | --- |
| 0 | `/etc/iqfiles/cam0` | `/dev/video29` | `/dev/video22` |
| 1 | `/etc/iqfiles/cam1` | `/dev/video38` | `/dev/video31` |

两个 IQ 目录中应分别存在：

```text
/etc/iqfiles/cam0/imx586_default_default.json
/etc/iqfiles/cam1/imx586_default_default.json
```

设备编号变化时，以 `media-ctl -p` 的实际 media graph 为准，并使用启动参数覆盖：

```text
--params0 DEVICE --params1 DEVICE --video0 DEVICE --video1 DEVICE
```

## 构建

在 SDK 主机中执行：

```sh
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart
make -f Makefile.camera_aiq aarch64
```

## 板卡运行顺序

1. 停止 `rkaiq_3A_server`，确认系统中只有本程序控制 RKAIQ。
2. 停止旧的 `v4l2-ctl`、`v4l2_frame_tap` 等采集进程。
3. 启动 `./camera_aiq_test`。
4. 输入 `stream-start all`，由本程序启动两路采集。
5. 等待两路出现 `CAMERA_STREAM ... STARTED` 和 `CAPTURE_STREAM ... STARTED`。
6. 使用 `status all` 同时查看 AIQ 和采集/保存状态。

```sh
./camera_aiq_test
```

不需要交互命令、直接提供 HTTP 地址时运行：

```sh
./camera_aiq_test --daemon
```

启动完成后可直接打开 `http://<board-ip>:8080/cam0`、
`http://<board-ip>:8080/cam1`，或用 `http://<board-ip>:8080/` 同时查看两路。
开机常驻配置和协议说明见 `NETWORK_HTTP_MJPEG.md`。

未接 MCU，同时常驻双路 UVC、双路 HTTP 和 RNDIS 网卡时运行：

```sh
./camera_aiq_test --all-daemon
```

`camera-uvc.service` 当前使用该模式。程序只启动一次两路采集，再把每一帧同时送给
UVC 和 HTTP；RNDIS Gadget 由 `usbdevice.service` 独立常驻。因此停止或恢复 UVC
送帧不会撤下 USB 网卡，电脑可继续通过 `192.168.55.1` 访问 HTTP 和 SSH。

双路 UVC 并连接最终 MCU 时运行：

```sh
./camera_aiq_test --all-daemon --uart /dev/ttyS9 --sync-timer-hz 1000000 \
  --xvs-autostart-hz 4 --xvs-low-pulse-us 10
```

接入 MCU 后再把该命令写入 `camera-uvc.service`。服务启动后，一个进程同时持有两路
相机、两路 UVC、网络和保存后端；UART 命令不会再启动第二个相机程序。未接 MCU 时
不要传 `--uart` 或 `--xvs-autostart-hz`，否则程序会因为 MCU 应答超时而退出。

不启动 UVC、只进入控制台联调同一个 MCU 时运行：

```sh
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

`--sync-timer-hz` 必须等于 MCU 上报 `timer_tick` 的实际计数频率。`--uart` 是推荐
参数：程序只打开一次 `/dev/ttyS9`，接收线程按 `$CAM`、带 CRC 的 `$ACK/$NACK`
和 `$EVT` 分流。相机控制在独立工作线程执行，因此 `$CAM ... SYNC_START` 可以继续
通过同一串口发送 `$XVS` 并等待 MCU 应答，不会阻塞接收线程。

`--control-uart`、`--sync-uart` 仅保留给旧脚本或确实有两路物理 UART 的产品。
当前没有 MCU、只验证相机控制时仍可单独使用 `--control-uart /dev/ttyS9`；接入
最终 MCU 后必须改用 `--uart /dev/ttyS9`。

## 本地命令

```text
status [all|0|1]
capture-status [all|0|1]
sync-status
sync-bind-reset [PRE_SHUTTER_TRIGGERS]
sync-bind-log CSV_PATH|off
sync-bind-status
sync-bind-last
sync-sim-start 2|4 [PULSE_COUNT]
sync-sim-stop
sync-sim-status
sync-controller-status
sync-idle
sync-start 2|4 [LOW_PULSE_US]
sync-count 2|4 PULSE_COUNT [LOW_PULSE_US]
sync-stop
time-sync-status
time-sync-reset

stream-start CAMERA_ID|all
stream-stop CAMERA_ID|all
save-start CAMERA_ID OUTPUT_DIR
save-stop CAMERA_ID

uvc-start CAMERA_ID|all
uvc-stop [CAMERA_ID|all]
uvc-status [CAMERA_ID|all]

net-start CAMERA_ID
net-stop [CAMERA_ID|all]
net-status [CAMERA_ID|all]

auto CAMERA_ID
exposure CAMERA_ID EXPOSURE_US
gain CAMERA_ID GAIN_X1000
fps CAMERA_ID FPS

wait MILLISECONDS
help
quit
```

参数说明：

- `camera_id` 为 `0` 或 `1`，所有控制和保存状态均按相机隔离。
- `exposure_us` 单位为微秒。设置曝光时先读取并保持当前实际模拟增益。
- `gain_x1000` 是 IMX586 传感器模拟增益，范围 `1000`～`64000`，`8000` 表示 8 倍。设置增益时先读取并保持当前实际曝光时间。
- ISO 由 RKAIQ 根据模拟增益、数字增益、ISP 增益和 IQ 计算，只在 `status` 中回读。
- `OUTPUT_DIR` 必须是没有空格的绝对路径。程序会创建不存在的目录。
- `save-start` 只在该路已经 `stream-start` 后成功。
- `save-stop` 停止接收新保存帧，并等待已经进入写盘队列的帧处理完成后返回。
- `uvc-start CAMERA_ID|all` 只能在对应相机已经出流后执行。`uvc-stop 0` 只停止
  camera0 的送帧与编码，camera1 和两个 USB 接口保持不变；主机未打开视频流时，
  `skipped_no_host` 增长是正常状态。完整抓图流程见
  `DUAL_UVC_4000X3000_TEST_20260731.md`。
- `uvc-stop all` 是软停止：只停止两路 UVC 编码和送帧，UVC 控制端点、configfs
  复合 Gadget 和 RNDIS 网卡继续存在，再执行 `uvc-start all` 可直接恢复且不会触发
  USB 重新枚举。`quit` 会关闭程序持有的 UVC 文件描述符和 HTTP 服务，但
  `usbdevice.service` 仍保留 Gadget/RNDIS。正常运行期间不要执行 `usbdevice stop`、
  `usbdevice restart` 或手工清空 Gadget 的 `UDC`，这些操作会让 RNDIS 和 UVC 一起掉线。
- `net-start CAMERA_ID` 要求对应相机已经出流，camera0/camera1 分别固定发布
  `http://<board-ip>:8080/cam0` 和 `http://<board-ip>:8080/cam1`。采集通常约
  30 fps，各路网络后端主动只编码每 100 ms 的最新一帧；因此 `queue_drops`
  增长属于预期，`encode_errors` 和 `http_errors` 必须保持为 0。旧 RTSP 版本的
  上板记录仍保留在
  `test_results/20260724/network_rtsp_cam1/RK3576_IMX586_CAM1_DUAL_RTSP_TEST_REPORT.md`，
  仅作为历史测试结果，不代表当前网络协议。
- `sync-status` 以两路当前最新帧中时间较新者为基准，在另一相机最近 32 帧中查找单调时间戳最接近的一帧并输出 `delta_ns`。只有两路都在采集且 `valid=1` 时，两路时间戳才具备同一单调时钟下的可比性。
- `time-sync-status` 输出 PPS/RMC 接收数量、UTC 锁定状态和 holdover 状态。
  最终照片验收必须是 `utc_valid=1`；`PPS_ONLY` 或 `UNLOCKED` 只能联调，不能
  作为准确 UTC 时间验收结果。完整协议与测试见
  `STAGE7_PPS_NMEA_TRIGGER_TIME_20260803.md`。

## 保存格式

当前第一版保存 ISP 输出的原始 NV12，每一帧一个文件。两平面的有效数据按 Y、UV 顺序拼接为标准 NV12：

```text
cam0_frame_0000000001_seq_0000001234_rt_1750000000000000000.nv12
```

文件名包含 `camera_id`、V4L2 `sequence` 和取帧时的系统实时时间。4000x3000 NV12 正常单帧大小应为：

```text
4000 * 3000 * 3 / 2 = 18000000 bytes
```

每个保存目录还会生成 `cam0_frames.csv` 或 `cam1_frames.csv`，只在对应 NV12 文件完整写入并重命名成功后追加一行：

```text
camera_id,file_index,v4l2_sequence,buffer_flags,v4l2_timestamp_ns,realtime_dequeue_ns,bytes,filename
```

- `v4l2_timestamp_ns` 来自内核 V4L2 buffer；`buffer_flags` 中时间戳类型为 `MONOTONIC` 时，才可跨两路比较。
- `realtime_dequeue_ns` 是应用层取到该 buffer 时的 `CLOCK_REALTIME`，可用于关联系统 UTC，但不是曝光起始时间。
- 当前时间戳描述的是 ISP/V4L2 帧事件。程序已经具备应用层 trigger/frame_id
  队列绑定；真正的触发因果与精度验收仍需 MCU 逐脉冲事件、物理 XVS、统一时钟和
  示波器测量。

采集线程只复制帧，独立写盘线程负责保存，避免慢存储直接阻塞 V4L2。每路保存队列最多缓存 4 帧；存储写入跟不上时，`save_queue_drops` 会增加，必须判定测试失败，不能只检查目录里是否有文件。

## 第一次板卡测试

先确认存储空间，4000x3000 NV12 每帧约 18 MB：

```sh
df -h /root
```

启动程序后输入：

```text
stream-start all
wait 3000
status all

fps 0 4
fps 1 2
wait 5000
status all
sync-status

save-start 0 /root/camera_test/cam0
wait 3000
save-stop 0
capture-status all

save-start 1 /root/camera_test/cam1
wait 3000
save-stop 1
capture-status all

stream-stop all
quit
```

板卡终端再检查：

```sh
find /root/camera_test -type f -name '*.nv12' -printf '%p %s bytes\n'
head -n 3 /root/camera_test/cam0/cam0_frames.csv
head -n 3 /root/camera_test/cam1/cam1_frames.csv
wc -l /root/camera_test/cam0/cam0_frames.csv \
      /root/camera_test/cam1/cam1_frames.csv
```

通过条件：

- 两路均能独立 `stream-start` 和 `stream-stop`；
- 两路 `frames` 持续增加，`fps_x1000` 与实际目标接近；
- cam0 保存时 cam1 仍连续采集，反向测试也一样；
- 保存文件均为 18000000 字节；
- `sequence_drops=0`、`save_queue_drops=0`、`save_failures=0`、`last_errno=0`；
- `save-stop` 后 `saving=0` 且 `save_queue_pending=0`；
- 修改一路曝光、增益或帧率时，另一路状态和画面没有非预期变化。
- `sync-status` 输出 `valid=1`；记录 `delta_ns` 的分布，但此项只验收软件时间戳链路，不作为硬件同步验收结论。

单路测试通过后，再同时执行：

```text
stream-start all
save-start 0 /root/camera_test/dual_cam0
save-start 1 /root/camera_test/dual_cam1
wait 5000
save-stop 0
save-stop 1
capture-status all
stream-stop all
```

双路同时保存时尤其关注 `save_queue_drops`。它直接反映当前 eMMC 实际写入能力是否足以承载两路全分辨率目标帧率。

## 硬件同步前置条件

当前 SDK 不能直接进入 IMX586 硬件 Trigger 驱动开发，原因如下：

- `kernel/drivers/media/i2c/imx586.c` 没有保存 `sync_mode`，也没有实现 `RKMODULE_GET_SYNC_MODE`、`RKMODULE_SET_SYNC_MODE` 或 FSIN/Trigger 寄存器配置。
- cam0/cam1 的 DTS 只有供电、时钟、复位和 MIPI endpoint，没有 Trigger/PPS GPIO、中断和同步模式属性。
- SDK 中没有附带当前 IMX586 模组的原理图、外触发管脚定义和供应商寄存器表。

开始修改内核前必须拿到并确认：

1. 当前两块 IMX586 模组是否将外部同步输入脚引出，以及连接器针脚号。
2. 输入电平、有效边沿、最小脉宽、允许频率和上电默认状态。
3. IMX586 当前 4000x3000 模式进入外部触发/从模式所需的完整寄存器表。
4. 两路同步输入如何连接：推荐同一个硬件 Trigger 经过合适的电平转换/扇出同时送入两颗 sensor，并另接 RK3576 GPIO 中断用于记录 `trigger_time`。
5. RK3576 上分配给 Trigger 和 PPS 的具体 GPIO；PPS 与拍照 Trigger 是两个不同信号，不能混用。

资料齐全后的实现顺序是：先单颗 IMX586 外触发出帧，再让同一 Trigger 驱动双颗，随后在驱动层记录 trigger/frame event，在本程序中绑定同一次 trigger 与 cam0/cam1 的 frame_id，最后才接 GPRMC/NMEA UTC 和同步开关。UART 协议层可以继续后置。

不要同时运行本程序和 `rkaiq_3A_server`。两个进程同时控制同一 ISP/传感器时，参数会互相覆盖，初始化或取帧也可能失败。
