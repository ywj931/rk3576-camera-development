# RK3576 双 IMX586 阶段 7 输出实现与验收

日期：2026-08-03

## 1. 实现结论

阶段 7 要求的三个输出后端已经接入同一个 `camera_aiq_test` 进程：

| 输出 | cam0 | cam1 | UART 独立控制 |
| --- | --- | --- | --- |
| Type-C UVC | `uvc.0`，MJPEG 4000x3000@10fps | `uvc.1`，MJPEG 4000x3000@10fps | 是 |
| 网口 HTTP | `http://板卡IP:8080/cam0` | `http://板卡IP:8080/cam1` | 是 |
| eMMC 保存 | 独立目录、NV12 + CSV | 独立目录、NV12 + CSV | 是 |

双 UVC 在 2026-07-31 已完成电脑端实测：两路同时以 10 fps 各传输 100 帧，
耗时分别约 10.18 秒和 10.21 秒；5 fps 各传输 50 帧也通过。详细原始记录见
`DUAL_UVC_4000X3000_TEST_20260731.md`。

本次补齐了两项：

1. `uvc-stop 0`、`uvc-stop 1` 和重新 `uvc-start`，停止一路不会断开另一路。
2. UART 不再使用 `CAMERA_BACKEND=NOT_CONNECTED` 的旧测试服务，而是直接调用
   同一进程中的相机、UVC、HTTP 和保存后端。

## 2. 为什么必须由一个进程完成

V4L2 采集节点和 RKAIQ context 不能由多套程序重复占用。统一进程的数据链是：

```text
IMX586 -> RKISP/RKAIQ -> V4L2 NV12M -> cam0/cam1 帧回调
                                      |-> UVC MPP MJPEG
                                      |-> HTTP MPP MJPEG
                                      |-> eMMC 异步 NV12 保存

MCU UART -> 协议解析 -> 同一个命令执行器 -> 上述三个输出后端
```

因此 UART 只负责传递“开启、停止、查询”等应用层命令，不直接写 IMX586 寄存器，
也不另外启动 `v4l2-ctl` 或第二个 `camera_aiq_test`。

## 3. UART 配置与协议

板端设备为 `/dev/ttyS9`，参数固定为 115200、8 data bits、no parity、1 stop bit，
无硬件/软件流控。程序用独占锁打开串口；若被串口终端或旧测试程序占用，会明确
返回 `UART is already in use`。

请求格式：

```text
$CAM,SEQUENCE,CAMERA_ID,COMMAND[,ARGUMENT...]\r\n
```

`CAMERA_ID` 为 `0`、`1`；需要同时控制两路或查询全局状态时使用 `255`。

常用请求：

```text
$CAM,1,255,PING
$CAM,2,255,GET_STATUS
$CAM,3,255,STREAM_START
$CAM,4,0,UVC_STOP
$CAM,5,0,UVC_START
$CAM,6,1,UVC_STATUS
$CAM,7,0,NET_START
$CAM,8,1,NET_STOP
$CAM,9,0,SAVE_START,/mnt/emmc/cam0
$CAM,10,0,SAVE_STOP
$CAM,11,0,EXPOSURE,30000
$CAM,12,1,GAIN,8000
$CAM,13,1,FPS,10
```

成功返回 `$ACK`，语法错误、状态错误或后端失败返回 `$NACK`。命令的完整输出放在
最后一个百分号编码字段内，避免状态文本中的逗号和换行破坏 UART 帧边界。

最终接口只有一个 `/dev/ttyS9`。使用 `--uart /dev/ttyS9` 时，一个 UART 接收线程
统一分流 `$CAM`、带 CRC 的 XVS ACK/NACK 和 `$EVT`；相机命令由独立工作线程执行，
可以在处理 `SYNC_START` 时通过同一串口继续与 MCU 事务通信。

## 4. 开机运行方式

`camera-uvc.service` 已配置为：

```text
/root/camera_uart/camera_aiq_test --uvc-daemon --uart /dev/ttyS9 --sync-timer-hz 1000000
```

它自动启动两路采集和两个 UVC 输出，然后等待 UART 命令。网络和 eMMC 可随后按
相机号开启。USB 配置脚本必须同时包含：

```text
UVC_INSTANCES="uvc.0 uvc.1"
UVC_CNT=2
```

## 5. 板端功能验收

更新文件后启动服务：

```sh
systemctl daemon-reload
systemctl restart camera-uvc.service
systemctl status camera-uvc.service --no-pager
journalctl -u camera-uvc.service -n 100 --no-pager
```

日志应有：

```text
UVC_AUTOSTART_READY cameras=0,1 outputs=2 mode=4000x3000@10fps/MJPEG
CONTROL_UART_READY device="/dev/ttyS9" baud=115200 format=8N1 protocol=CAM_V1
UART_MUX_READY device="/dev/ttyS9" baud=115200 format=8N1 routes=CAM,XVS_ACK,PPS_NMEA_TRIGGER
```

UART 依次测试 PING、两路状态、分别停止/启动 UVC、分别启动/停止网络和保存。
每次发送后必须收到相同 `SEQUENCE` 和 `CAMERA_ID` 的 ACK；无响应、NACK、序号错配
都判失败。

## 6. 电脑端双 UVC 验收

连接 Type-C device/OTG 数据口后，先识别两个视频采集节点：

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video2 --list-formats-ext
```

节点编号可能变化，不能永久假定是 0 和 2；同一 UVC 接口通常还会枚举一个不能
取图的 metadata 节点。

两路同时抓取 100 帧：

```sh
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --set-parm=10 --stream-mmap=4 --stream-count=100 --stream-to=/dev/null &
PID0=$!
v4l2-ctl -d /dev/video2 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --set-parm=10 --stream-mmap=4 --stream-count=100 --stream-to=/dev/null &
PID1=$!
wait "$PID0"
wait "$PID1"
```

然后通过 UART 执行 `UVC_STOP` camera0。预期 cam0 停止收到新帧，cam1 仍连续
取帧，电脑的两个 USB 摄像头接口不重新枚举；执行 camera0 `UVC_START` 后 cam0
恢复。再交换 camera0/camera1 重复测试。

## 7. 最终通过条件

- 电脑枚举两个标准 UVC 视频接口，画面分别来自 cam0、cam1，无串路。
- 两路 4000x3000@10fps 同时运行 1000 帧，无编码错误和服务重启。
- 分别停止/启动一路 UVC 时另一路不中断，USB 不重新枚举。
- HTTP 两路能分别开启、停止、查询，URL 和 camera_id 对应正确。
- eMMC 两路能分别保存，单帧 NV12 为 18,000,000 字节，CSV 与文件数一致。
- `sequence_drops=0`、`save_queue_drops=0`、`save_failures=0`、
  `encode_errors=0`。
- UART 每条命令均返回匹配的 sequence/camera_id，不存在超时或错误 ACK。

## 8. 2026-08-03 本版本上板结果

部署程序：

```text
/root/camera_uart/camera_aiq_test
SHA-256: 4f344e04072799327057f0277ef37d56dfe99dca71aedea6c0f52792bda115b6
```

本次已经完成：

- AArch64 交叉编译通过；电脑端 UART 解析、伪终端 115200/8N1 收发和 ACK 测试
  通过，合计 14 个相机协议用例。
- 板端 `--control-uart-protocol-self-test` 通过；正式服务成功独占打开
  `/dev/ttyS9`，日志出现 `CONTROL_UART_READY`。
- 正式服务识别 `uvc.0`、`uvc.1`，板端节点为 `/dev/video49`、
  `/dev/video50`，日志出现 `outputs=2`。
- 板端先启动两路采集，再执行 `uvc-start all`，两路均为 `enabled=1`。
  执行 `uvc-stop 0` 后 cam0 为 `enabled=0`、cam1 仍为 `enabled=1`；恢复
  cam0 成功。cam1 独立停止和恢复也通过。
- HTTP cam0、cam1 同时工作；板内访问两个 URL 均返回 HTTP 200，3 秒分别接收
  21,542,677 字节和 20,392,055 字节。首页返回 HTTP 200。
- eMMC 独立保存路径、NV12 文件和 CSV 创建成功：cam0 保存 41 帧、
  738,000,000 字节，cam1 保存 33 帧、594,000,000 字节；无写文件失败。
  测试产生的 1.3 GiB 临时原始文件已清理。
- 当时测试结束后恢复的旧服务参数为
  `--uvc-daemon --control-uart /dev/ttyS9`；统一 UART 版本改为
  `--uvc-daemon --uart /dev/ttyS9 --sync-timer-hz 1000000`，需接入 MCU 后上板回归。

本次尚未完成的最终硬件验收：

- 电脑当前只枚举到 CH340 调试串口，Type-C device/OTG 数据口未连接，因此新版
  程序尚未在电脑端重新完成双 UVC 取图和按路停止测试。2026-07-31 的旧版本双路
  同时取图结果仍然有效，但不能替代本版本回归。
- 没有外部 MCU/串口对端，本次没有从真实 UART 线发送阶段 7 命令；已验证实际
  串口打开、板端协议自测和电脑伪终端双向收发。接入 MCU 后仍需逐条做 ACK 验收。
- 本次相机自由运行约 24 fps，原始 NV12 高速写盘时 cam1 出现 8 次保存队列丢弃。
  阶段 5 的正式工作帧率为 2 Hz/4 Hz，必须在该帧率下复测 1000 帧，并达到
  `save_queue_drops=0`。
- 双 UVC 1000 帧、UART 实线、HTTP/UVC/eMMC 并发压力仍需完成，才能把阶段 7
  标记为最终验收通过。
