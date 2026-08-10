# RK3576 双 IMX586 v0.2 第三、四部分测试报告

测试日期：2026-08-04  
测试对象：RK3576 + 两路 IMX586，4000x3000  
依据文档：`3576 MIPI 相机模组任务计划 v0.2`  
当前硬件条件：未连接 MCU，未提供外部 PPS/GPRMC/XVS；板卡通过同一根 USB 线以
RNDIS + 双 UVC 复合设备连接电脑，实际协商速率为 USB 2.0 High-Speed（480 Mbps）。

## 1. 结论先行

本次已经把新内核、应用和 UVC 配置刷入板卡，板卡正常启动。第三部分的
PPS/GPRMC 时间状态机、Trigger 与双路 frame_id 绑定、JPEG EXIF/UserComment
写入均通过软件自检，但没有 MCU 和真实 XVS，不能判定真实硬件同步通过。

第四部分不能整体判为通过。两路 RAW10/YUV 能力、双 UVC 端点、RNDIS 网口以及
独立 2/4 Hz 寄存器配置均已确认；RNDIS 和双 UVC 可以同时枚举，电脑能访问板端
HTTP 服务，也能同时让两个 UVC 端点进入 streaming 状态。但相机已工作在 XVS 从
模式，没有外部 XVS 时实际帧数为 0，所以 HTTP 和 UVC 均没有图像负载。当前程序
还不接受外部 1 Hz 命令，长稳、真实端到端视频和全功能功耗也没有满足实测条件。

状态定义：

- **通过**：本次已获得可重复的实际或软件测试结果。
- **静态通过**：驱动节点、格式、配置或寄存器回读正确，但没有真实数据流。
- **部分通过**：逻辑中的一部分有证据，仍缺少文档要求的关键验证。
- **环境阻塞**：缺少 MCU、外部脉冲、USB 枚举或测量设备，当前无法执行。
- **不支持**：当前程序没有该能力，需要修改后再测。

## 2. 刷入版本和回滚点

板卡启动版本：

```text
Linux localhost 6.1.99 #10 SMP Tue Aug 4 15:09:09 CST 2026 aarch64
```

已刷入文件：

| 文件 | SHA-256 |
|---|---|
| 本地 `kernel-6.1/boot.img` | `810dbad6295b404148d5214100e13aeb20b0458bee7cce8a3be28814d2e4e540` |
| 板端 `/root/camera_uart/camera_aiq_test` | `1e8c22dafad98f01ca4ac6c46b0cc3ccaa6f776cfb4ce33b2afb66710daef719` |
| 板端 `/usr/bin/usbdevice` | `f79fd7d247cf4ecab4e58e9eb1950acc6d49396d813de9678b953386182bc0aa` |
| 板端 `/etc/systemd/system/camera-uvc.service` | `2b44e31895949f6f65de68855c77bed628373a7d3b9f9c048ac8e17084199118` |

刷入前完整备份位于：

```text
/root/camera_uart/backups/20260804_before_independent_xvs_runtime/
```

其中包括原 boot 分区镜像、旧应用、旧 USB 脚本、旧 service 和 SHA256SUMS。

## 3. 第三部分：同步与时间戳逐项检查

| 文档步骤 | 当前实现和测试证据 | 结果 |
|---|---|---|
| 1. 捕获 PPS 上升沿并记录高精度计数 | 程序接收 MCU 上报的 `PPS` 事件和 1 MHz `timer_tick`；模拟协议通过。未接 MCU，未测真实边沿。计时来自 MCU 定时器，不是 RK3576 GPIO 直接捕获。 | 部分通过 |
| 2. PPS 后解析 GPRMC/NMEA | Mock UART 覆盖 `PPS/RMC/NMEA/XVS`，GPRMC 和 GNRMC 解析测试通过。 | 软件通过 |
| 3. 下一 PPS 使用上条 GPRMC + 1 秒 | `TIME_SYNC_SERVICE_TEST_OK` 和 `STAGE7_TIME_PIPELINE_TEST_OK pps_to_utc=verified`。 | 软件通过 |
| 4. 从 PPS 用高精度计数继续计时 | 1 MHz tick 到 ns 的换算和 HOLDOVER 状态机通过；未测试 MCU 晶振误差、漂移和长时间保持。 | 部分通过 |
| 5. 触发曝光时记录 trigger_time | XVS UART 协议和模拟 Trigger 生成通过；真实 XVS 未输入。 | 部分通过 |
| 6. 计算 exposure_us/start/center | 程序用曝光值和每路 response offset 计算 start/center，EXIF 自检通过；当前曝光值来自出帧时 RKAIQ 最新查询，不是按该 frame_id 锁存的 sensor 曝光寄存器和 line time。 | 部分通过 |
| 7. 绑定 frame_id/camera_id/曝光/增益/时间 | 模拟绑定通过，结果为两路 trigger/frame 配对且 `frame_delta_ns=200000`；真实双帧为 0，未做硬件绑定。 | 软件通过，硬件未测 |

### 3.1 本次软件测试原始结论

板端：

```text
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14"
XVS_PROTOCOL_SELF_TEST_OK
SYNC_BIND_SELF_TEST_OK source=SIM triggers=2 pairs=2 frame_delta_ns=200000
PHOTO_EXIF_SELF_TEST_OK detail="JPEG APP1/EXIF UTC and trigger UserComment verified"
```

电脑端：

```text
XVS_UART_MOCK_TEST_OK one_uart_mux=CAM,XVS_ACK,EVT async_events=PPS,RMC,NMEA,XVS
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14 pty=115200_8N1_ACK"
TIME_SYNC_SERVICE_TEST_OK state=HOLDOVER utc_valid=0 rmc_variants=GNRMC,GPRMC
STAGE6_HOST_TEST_OK exif="JPEG APP1/EXIF UTC and trigger UserComment verified"
STAGE7_TIME_PIPELINE_TEST_OK pps_to_utc=verified trigger_id=9001 cam0_frame_id=1701 cam1_frame_id=2701
```

### 3.2 第三部分尚不能签字的两点

1. **真实时间源未验收。** 没有 MCU，无法证明 PPS 边沿、NMEA 秒标和 XVS 使用同一
   时间域，也无法测量 UTC 误差和长期漂移。
2. **曝光参数没有与 frame_id 硬绑定。** Trigger 和 V4L2 sequence 的绑定逻辑存在，
   但 `exposure_us/gain/ISO` 是 DQBUF 时查询 RKAIQ 当前值。固定手动曝光时通常一致，
   自动曝光或快速切换参数时不能证明该值一定属于这一帧。

要严格符合文档第 6、7 步，应在 sensor/ISP 帧事件处保存每帧曝光寄存器结果，形成
`frame_id -> exposure lines/gain/line_time` 快照，再交给照片元数据模块使用。

## 4. 第四部分：帧率测试记录表结果

| 测试项 | 本次检查 | 结果 |
|---|---|---|
| 4000x3000 RAW10 | cam0 `/dev/video0`、cam1 `/dev/video11` 均枚举 RG10/BA10/GB10/BG10；media bus 为 `SRGGB10_1X10/4000x3000`。无外部 XVS，未获得 RAW 帧。 | 静态通过 |
| 4000x3000 YUV | cam0 `/dev/video22`、cam1 `/dev/video31` 均支持 NV12/NM12/UYVY 等，最大 4000x3000；实际 `frames=0`。 | 静态通过 |
| 4000x3000 JPEG | JPEG/EXIF 生成器自检通过；本次没有真实 NV12 输入，因此没有新拍摄 JPEG。 | 软件通过，实拍阻塞 |
| 极限 FPS、丢帧和资源 | 没有帧，无法测平均 FPS、丢帧和运行负载。启动等待 XVS 期间内核记录 3 次 MIPI CSI2 ERR2。 | 环境阻塞 |
| 最高 FPS 30-60 分钟长稳 | XVS 从模式下无外部脉冲，不能开始。 | 未测试 |
| 外部 1 Hz | 当前 `sync-start`、`sync-count` 和 MCU 控制器只接受 2 或 4。 | 不支持 |
| 外部 2 Hz/4 Hz | 协议自检通过；真实 MCU/XVS 未接。 | 软件通过，硬件未测 |
| 双路独立 2/4 Hz | `fps 0/1 2/4` 可分别设置 sensor 的 `xvs_input_thin`：0=每个 4 Hz XVS 出帧，1=隔一个 XVS 出帧。未切换相机的配置不变；无 XVS 时无法测实际 FPS。 | 静态通过 |
| Type-C 双 UVC | configfs 同时存在 `uvc.0`、`uvc.1`，两路都声明 2500000/5000000（100 ns 单位，即 4/2 Hz）。电脑枚举出两路视频节点并同时完成 4000x3000@4 Hz/MJPEG 协商；因源帧为 0，电脑端两个输出文件均为 0 字节。 | 枚举和协商通过，实流阻塞 |
| USB 网口输出 | 同一复合 Gadget 枚举 RNDIS，电脑获得 `192.168.55.19/24`，可 ping 通板端 `192.168.55.1`；HTTP 首页返回 200 和 592 字节。`/cam0`、`/cam1` 均返回 200 multipart 响应头，但没有图像帧。 | 网络链路通过，实流阻塞 |
| UVC + 网口同时工作 | `rndis.0 + uvc.0 + uvc.1` 同时绑定；两路 UVC `host_streaming=1` 时 HTTP 服务仍监听并接受两路客户端，没有端口或功能冲突。 | 控制面通过，数据面阻塞 |
| UVC + 网口 + eMMC + YOLO 全功能 | 本次同时启用了 stream、双 HTTP、双 UVC、双 save/photo，功能均能进入运行态；但源帧为 0，未形成真实编码、保存或 YOLO 负载，不能代替组合压力和功耗测试。 | 部分通过 |

### 4.1 无 MCU 时的真实帧率结果

交互运行程序后执行：

```text
stream-start all
wait 5000
capture-status all
```

两路共同结果：

```text
running=1
frames=0
fps_x1000=0
timestamp_valid=0
```

这不是 V4L2 采集程序故障。新驱动已把两颗 IMX586 配成 XVS 从模式，sensor 在收到
外部 XVS 前不会输出一帧，CSI/ISP 因而也没有数据可取。

执行 `fps 0 2` 后，cam0 的 `xvs_input_thin` 从 0 变为 1，cam1 保持 0，证明独立
配置路径正确；命令最后报告 `target fps did not stabilize`，因为两路实际帧数仍为 0。
恢复 `fps 0 4` 后 cam0 回读为 0。

### 4.2 USB 网口与双 UVC 复合模式实测

板端：

```text
UDC=/sys/class/udc/23000000.usb
UDC_STATE=configured
UDC_CURRENT_SPEED=high-speed
CONFIG_FUNCTIONS=rndis.0,uvc.0,uvc.1
```

电脑端实测：

```text
USB device: 2207:0017 Fuzhou Rockchip rk3xxx
USB topology: 4 x Video interface + RNDIS control/data, 480M
RNDIS host: enx069223379382 = 192.168.55.19/24
Board RNDIS: usb0 = 192.168.55.1/24
UVC pixel nodes: /dev/video0, /dev/video2
UVC metadata nodes: /dev/video1, /dev/video3
```

电脑到板卡 RNDIS ping 为 4/4、0% 丢包、平均约 0.272 ms。浏览器入口对应的 HTTP
首页 `http://192.168.55.1:8080/` 返回 `HTTP 200`，下载 592 字节，不需要 SSH 端口
转发。两路图像端点 `/cam0` 和 `/cam1` 均返回：

```text
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace;boundary=frame
```

但等待 6 秒后两个请求都收到 0 字节图像载荷。电脑同时打开 `/dev/video0` 和
`/dev/video2` 后，板端两路状态均为：

```text
enabled=1 host_streaming=1 negotiated=4000x3000@4fps/MJPG
submitted=0 encoded=0 sent=0
```

同时板端 `capture-status all` 为两路 `frames=0`，所以空 HTTP/UVC 的直接原因是没有
XVS 源帧，不是复合 USB、RNDIS、HTTP 路由、UVC 枚举或格式协商失败。

本次复测使用的最小流程如下。板端进入 `camera-aiq>` 后保持程序运行：

```text
stream-start all
net-start 0
net-start 1
uvc-start all
wait 5000
capture-status all
net-status all
uvc-status all
```

电脑端确认并测试：

```bash
ip -brief address
ping -c 4 192.168.55.1
curl -m 5 http://192.168.55.1:8080/
curl -m 6 http://192.168.55.1:8080/cam0 -o /tmp/cam0_http.mjpg
curl -m 6 http://192.168.55.1:8080/cam1 -o /tmp/cam1_http.mjpg
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=4 --stream-to=/tmp/cam0_uvc.mjpg
v4l2-ctl -d /dev/video2 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=4 --stream-to=/tmp/cam1_uvc.mjpg
```

`/dev/videoN` 编号由电脑动态分配，重新插拔后必须先用 `v4l2-ctl --list-devices`
确认，不能永久写死为 0 和 2。

本次 USB 仍只有 `high-speed/480M`，没有达到任务所需的 USB 3.0 SuperSpeed。
`maximum_speed=super-speed` 只表示控制器允许 USB 3.x，不代表物理链路已经以 USB
3.x 连接；正式通过时板端 `current_speed` 必须为 `super-speed`，电脑端 RK3576
设备必须显示 5000M 或更高。当前拓扑中 RK3576 位于 USB 2.0 Hub 的 480M 分支，
正式带宽测试应使用具备 SuperSpeed 数据对的线材并直连电脑 USB 3.x 端口。

### 4.3 复合模式下的程序状态

在一个 `camera_aiq_test` 实例中依次启用：

```text
stream-start all
net-start 0
net-start 1
uvc-start all
save-start 0 /tmp/cam0
save-start 1 /tmp/cam1
photo-start 0 /tmp/photo_cam0
photo-start 1 /tmp/photo_cam1
```

所有功能均能进入 enabled/running 状态，没有监听端口冲突或设备占用冲突。无 XVS
条件下，采集、HTTP、UVC、保存和拍照计数全部保持 0。程序此时约占 1.0% CPU，RSS
约 84 MiB，整机内存 3896 MiB、used 814 MiB、available 3081 MiB；这是“全部后端已
开启但没有视频帧”的资源值，不能当作满负载数据。

2026-08-04 已修正 UVC 停止生命周期。`uvc-stop all` 现在只停止两路编码和送帧，
不会调用 UVC 控制层退出，也不会解绑 configfs Gadget。板端复测中该命令返回
`usb_gadget=kept rndis=kept`；停止后 `f-rndis.0`、`f-uvc.0`、`f-uvc.1`、UDC
绑定和 `usb0=192.168.55.1/24` 均保留，已启动的 cam0/cam1 HTTP 服务仍为
`server_running=1`；再次执行 `uvc-start all` 可直接恢复，不重新打开 UVC 节点。
程序 `quit` 后 `usbdevice.service` 仍为 active，复合 Gadget/RNDIS 仍绑定；HTTP
服务属于相机程序，因此会随程序退出。

本次修改后复测时，当前连接电脑没有枚举出该 RNDIS 设备，板端 `usb0` 为
`NO-CARRIER`，所以无法补做电脑端“停止 UVC 期间持续 ping”的数据面证明。板端生命周期
和同进程 HTTP 连续性已经验证；连接恢复后还需按自测手册第 12 节完成连续 ping，才可
把“物理链路不掉包”记为通过。正常运行不得执行 `usbdevice stop/restart` 或手工解绑
UDC，这些操作仍会使整个复合 USB 重新枚举。

### 4.4 当前资源基线

这是本轮启动前记录的无视频流空闲基线，不能代替第四部分负载数据：

| 指标 | 本次值 |
|---|---|
| 内存 | 3896 MiB 总量，184 MiB 已用，3711 MiB available，无 swap |
| Load average | 0.00 / 0.00 / 0.00 |
| SoC 温度 | 43.461 C |
| 大核/小核温度 | 45.307 C / 45.307 C |
| DDR 温度 | 44.384 C |
| NPU 温度 | 43.461 C |
| GPU 温度 | 45.307 C |
| MIPI 错误 | 3 次 CSI2 ERR2，均出现在无 XVS 的启动/等待过程 |

## 5. 接上 MCU 后的正式测试流程

### 5.1 测试前条件

1. MCU 串口接到 `/dev/ttyS9`，参数为 115200、8N1。
2. MCU 的 PPS、XVS 和串口事件必须共用可换算的 1 MHz timer tick。
3. MCU 发送有效 GPRMC/GNRMC，并按协议返回 XVS 命令 ACK。
4. 示波器同时测 MCU XVS、cam0 XVS 引脚和 cam1 XVS 引脚。
5. USB 数据线直连电脑 USB 3.x 端口；正式测速不要经过只支持 480M 的 Hub。

### 5.2 交互启动

板端自动服务当前为 `enabled` 但测试时已停止，因为无 MCU 会反复报
`MCU response timeout`。接好 MCU 后先用交互方式验收：

```bash
systemctl stop camera-uvc.service
cd /root/camera_uart
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

进入 `camera-aiq>` 后：

```text
stream-start all
sync-bind-reset 0
sync-bind-log /tmp/sync_bind.csv
sync-start 4 10
wait 5000
time-sync-status
sync-controller-status
capture-status all
sync-bind-status
```

判定条件：两路 `frames` 持续增长，`fps_x1000` 非 0，PPS/RMC 计数增长，
`utc_valid=1`，trigger 能分别找到 cam0/cam1 frame_id。

### 5.3 按文档第四部分测试帧率

先验证共享外部 4 Hz 下两路任意独立切换：

```text
fps 0 4
fps 1 4
wait 10000
capture-status all
sync-bind-status

fps 0 4
fps 1 2
wait 10000
capture-status all
sync-bind-status

fps 0 2
fps 1 4
wait 10000
capture-status all
sync-bind-status

fps 0 2
fps 1 2
wait 10000
capture-status all
sync-bind-status
```

10 秒窗口的期望增量：4 Hz 约 40 帧，2 Hz 约 20 帧。允许启动边界相差 1 帧，
不允许切换 cam0 时 cam1 停流或配置被改变，反之亦然。

然后分别测试外部 2 Hz 和 4 Hz：

```text
sync-stop
sync-start 2 10
wait 10000
capture-status all
sync-bind-status

sync-stop
sync-start 4 10
wait 10000
capture-status all
sync-bind-status
```

注意：当前“独立 2/4 Hz”方案以共享 4 Hz XVS 为基准，用 sensor thin 实现每路 4
或 2 Hz。若把外部 XVS 改为 2 Hz，thin=1 的相机会变成约 1 Hz。因此外部频率测试
和两路独立 2/4 Hz 测试是两种不同工况，报告中必须分别记录。

外部 1 Hz 暂时不能用 `sync-start 1`。正式执行文档 1 Hz 项目前，需要同时增加 MCU
协议、`xvs_uart_controller.cpp`、命令校验和状态输出对 1 Hz 的支持。

### 5.4 EXIF 与帧 ID 验收

```text
photo-start 0 /tmp/photo_cam0
photo-start 1 /tmp/photo_cam1
wait 10000
photo-stop 0
photo-stop 1
photo-status all
sync-bind-last
```

对每张 JPEG 和 CSV 核对：

- `camera_id` 与目录一致；
- `frame_id` 等于对应 V4L2 sequence；
- `trigger_id` 在左右帧中相同；
- `DateTimeOriginal/SubSecTimeOriginal` 对应 `exposure_start_realtime_ns`；
- `ExposureTime` 与该帧曝光快照一致；
- `PhotographicSensitivity` 与该帧 ISO/gain 映射一致；
- `UserComment` 包含 trigger、PPS、frame、start、center 的纳秒值；
- 100 帧时间戳完整率应为 100%。

### 5.5 双 UVC 验收

板端在数据流稳定后：

```text
uvc-start all
uvc-status all
```

另一个板端终端检查：

```bash
cat /sys/class/udc/23000000.usb/state
cat /sys/class/udc/23000000.usb/current_speed
```

电脑端先用 `v4l2-ctl --list-devices` 找到两路新 UVC 节点，再分别和同时采集。不要
预先假设节点一定是 `/dev/video0`、`/dev/video1`。

```bash
v4l2-ctl -d /dev/videoX --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=240 --stream-to=/tmp/cam0.mjpg

v4l2-ctl -d /dev/videoY --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=2 --stream-mmap=4 --stream-count=120 --stream-to=/tmp/cam1.mjpg
```

必须记录两路实际 FPS、超时/丢帧、60 分钟保存结果、电脑端 USB 速率和板端温度。

## 6. 最终判定

不能把当前状态写成“v0.2 第三、四部分全部通过”。准确结论如下：

- 新程序和 XVS 从模式内核已经刷入，双 sensor、RAW10/YUV 节点、独立 2/4 Hz
  配置、RNDIS 网络链路、双 UVC 枚举与格式协商通过。
- 第三部分的软件逻辑测试通过；真实 PPS/GPRMC/XVS、UTC 精度、左右同步误差以及
  帧级曝光参数仍待 MCU 实测。
- 同一根 USB 线可同时提供 RNDIS 网口和双 UVC，HTTP 首页可以直接通过
  `192.168.55.1:8080` 访问；无 XVS 时两种输出都没有图像载荷。
- 第四部分的真实帧率、长稳、UVC/HTTP 图像端到端和全功能功耗未完成；USB 实际
  为 480M，USB 3.0 也未通过。
- 文档要求的外部 1 Hz 当前不受程序支持，属于确定的软件缺口。
- 当前没有画面的直接原因是两颗 IMX586 等待外部 XVS，不是双路 V4L2 节点消失。
