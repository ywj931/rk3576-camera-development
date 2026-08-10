# RK3576 双 IMX586 v0.2 详细自测手册

> **历史版本警告（2026-08-05）：** 本文按早期 XVS从模式状态编写。当前板卡已切换为
> 自由运行设备树，未接 MCU也会真实出图；不要再按本文的 `frames=0`预期判定。
> 当前无 MCU测试请使用
> `RK3576_IMX586_V02_NO_MCU_SELF_TEST_GUIDE_20260805.md`。

文档版本：V1.0  
编写日期：2026-08-04  
适用硬件：RK3576 + 两路 IMX586，4000x3000  
适用程序：`/root/camera_uart/camera_aiq_test`  
测试依据：`3576 MIPI 相机模组任务计划 v0.2`  
文档用途：供不了解本项目的测试人员按顺序完成自测、记录证据并作出正确结论。

---

## 1. 测试前必须知道的结论

当前两颗 IMX586 已配置为 **XVS 从模式**。传感器只有收到外部 XVS 脉冲才输出
图像。未接 MCU 时执行 `stream-start all` 后两路 `frames=0` 是当前设计的预期
现象，不代表 V4L2、HTTP 或 UVC 本身故障。

本手册分为两条路径：

| 路径 | 硬件条件 | 能验证什么 | 不能验证什么 |
|---|---|---|---|
| A：未接 MCU | 当前板卡、双相机、电脑 | 程序自检、双 sensor/IQ 静态配置、参数写入路径、USB 枚举、RNDIS、HTTP 首页、双 UVC 协商 | 真实帧率、真实画面、硬件同步、PPS/UTC、实拍 EXIF、满负载稳定性 |
| B：接 MCU | 增加 MCU、XVS、PPS/GNSS、示波器 | 双路 2/4 Hz、独立参数、真实 UVC/HTTP/eMMC、trigger/frame 绑定、UTC、同步和长稳 | 只有完成全部步骤后才能签署整机验收 |

测试结果只能使用以下五种状态：

- **通过**：有本次真实数据或软件自检证据，所有判定条件满足。
- **静态通过**：节点、格式或寄存器配置正确，但没有真实图像帧。
- **软件通过**：模拟器/单元测试通过，不代表外部硬件通过。
- **环境阻塞**：缺少 MCU、XVS、PPS、USB 3.x 链路或测量仪器。
- **不通过**：具备测试条件，但结果不满足判定值。

禁止把“命令返回 OK”“UVC 已枚举”或“HTTP 返回 200”单独写成真实视频通过。
必须同时看到帧计数增长和输出文件中有有效图像。

### 1.1 原需求 1～7 与本手册的对应关系

| 原需求阶段 | 主要内容 | 本手册章节 |
|---:|---|---|
| 1 | 两路 IMX586、双采集链路 | 第 6 节 |
| 2 | 双 ISP/IQ、曝光和增益/ISO 独立 | 第 6、7 节 |
| 3 | 两路 2/4 Hz 独立切换 | 第 7 节 |
| 4 | UART 115200 8N1 控制 | 第 8 节 |
| 5 | XVS、硬件同步和帧计数 | 第 9 节 |
| 6 | PPS/GPRMC、trigger/frame、JPEG/EXIF | 第 10、11 节 |
| 7 | eMMC、双 UVC、网口和并发输出 | 第 11、12 节 |

任务文档第四部分要求的 RAW10、最大 FPS、长稳、USB 3.0、资源和功耗分别放在
第 7.6、12、13 节，不能因阶段 1～7 的程序功能已实现而省略实测。

### 1.2 当前版本的已知基线

截至 2026-08-04 已确认：当前程序哈希见第 4.2 节；复合 USB 已同时枚举 RNDIS、
`uvc.0` 和 `uvc.1`；RNDIS 链路及 HTTP 首页可访问。当前物理 USB 链路实测仍为
`high-speed/480M`，不是 USB 3.0；未接 MCU/XVS 时两路源帧均为 0，所以 HTTP 和
UVC 没有真实图像载荷。这些是测试起点，不是整机最终通过结论。

---

## 2. 终端和接口说明

本文所有命令前都标明执行位置：

| 标记 | 在哪里执行 | 提示符示例 |
|---|---|---|
| `【电脑端】` | Ubuntu 电脑终端 | `ywj@ywj:~$` |
| `【板卡 Shell】` | RK3576 Linux 命令行 | `root@localhost:~#` |
| `【camera-aiq>】` | `camera_aiq_test` 的交互界面 | `camera-aiq>` |
| `【MCU】` | MCU 固件或 MCU 串口日志 | 由 MCU 工程决定 |

### 2.1 两个串口不能混用

| 链路 | 设备 | 串口参数 | 用途 |
|---|---|---|---|
| 电脑调试串口 | 电脑上的 `/dev/ttyUSB0`，编号可能变化 | **1500000** | 登录板卡 Linux |
| RK3576 与 MCU 控制串口 | 板卡上的 `/dev/ttyS9` | **115200、8N1、无流控** | 相机命令、XVS ACK、PPS/NMEA/XVS 事件 |

`/dev/ttyUSB0` 能登录板卡，不代表 `/dev/ttyS9` 已通过 MCU 实线测试。

### 2.2 camera_id 固定含义

| camera_id | Sensor | IQ 文件 | 当前 ISP/YUV 采集节点参考值 |
|---:|---|---|---|
| 0 | `m00_b_imx586 4-001a` | `/etc/iqfiles/cam0/imx586_default_default.json` | `/dev/video22` |
| 1 | `m01_b_imx586 5-001a` | `/etc/iqfiles/cam1/imx586_default_default.json` | `/dev/video31` |

设备节点编号可能因内核和 media graph 变化。每次测试都要从程序的
`CAMERA_INIT` 日志和 `capture-status all` 确认，不能永久写死。

### 2.3 参数单位

| 参数 | 命令示例 | 含义和范围 |
|---|---|---|
| 曝光 | `exposure 0 5000` | 5000 us，程序范围 1～1000000 us |
| 模拟增益 | `gain 0 2000` | 2.000 倍，范围 1000～64000 |
| ISO | `iso 0 100` | 当前按基准 ISO 50 映射，范围 50～3200 |
| 帧率 | `fps 0 4` | 目标 4 Hz，当前 XVS 独立模式只验收 2 或 4 Hz |

`gain` 和 `iso` 是设置同一个曝光增益目标的两种入口。基准 ISO 为 50 时，
`gain 2000` 与 `iso 100` 对应同一个约 2 倍模拟增益；后执行的命令覆盖前一个增益
目标。不要把 `gain 8000` 理解成 ISO 8000。

`status` 中：

- `aiq_iso` 是 RKAIQ 原始查询结果；当前版本可能为 0。
- `iso` 是程序用于控制和元数据的有效 ISO。
- `iso_estimated=1` 表示 ISO 由模拟、数字、ISP 增益和基准 ISO 50 换算。
- 若产品规范强制要求 RKAIQ 原生 ISO，`aiq_iso=0` 仍不能判为原生 ISO 通过。

---

## 3. 测试设备和接线

### 3.1 必备设备

- RK3576 板卡和稳定的独立电源。
- 两颗连接正确的 IMX586。
- 可传数据的 USB Type-C 数据线。
- Ubuntu 电脑，安装 `v4l-utils`、`ffmpeg`、`usbutils`、`curl`、`exiftool`。
- 完整验收时增加 MCU、GNSS/PPS 源和四通道示波器或逻辑分析仪。
- 功耗验收时增加外置直流功率计。

### 3.2 MCU 接线

| RK3576 CN4 | MCU | 作用 |
|---|---|---|
| pin 19 `UART_CAM1_TX` | MCU UART RX | RK3576 发命令 |
| pin 17 `UART_CAM1_RX` | MCU UART TX | MCU 返回 ACK 和事件 |
| pin 23 `FSYNC_CAM` | MCU 硬件定时器/PWM 输出 | XVS，空闲高、低脉冲有效 |
| GND | MCU GND | 必须共地 |

**电气警告：** UART9 所在 VCCIO 为 1.8 V。MCU 为 3.3 V 时必须加合适的电平
转换，不能把 3.3 V UART 或 RS-232 电平直接连接到 RK3576 引脚。XVS 电平也要按
原理图和 U9803 输入规格核实。

### 3.3 XVS 和 MCU 基本要求

- XVS 使用 MCU 硬件定时器/PWM，不允许在主循环里延时翻转 GPIO。
- 正式共享时基为 4 Hz：周期 250000 us，建议低脉宽 10 us，空闲为高。
- 两颗 IMX586 接收同一条 4 Hz XVS。
- 每路的 2 Hz 由该传感器自己的 `xvs_input_thin=1` 实现，不改变另一颗相机。
- `PPS.timer_tick` 和 `XVS.timer_tick` 必须来自同一个不复位的高精度计数器。
- MCU 必须按 `MCU_XVS_UART_PROTOCOL.md` 返回带 CRC16 的 ACK/NACK 和异步事件。

---

## 4. 测试记录目录和版本确认

第一次可直接使用 `RUN001`；重复测试时改为 `RUN002`，不要把两次结果混在一起。

### 4.1 建立目录

`【板卡 Shell】`

```bash
mkdir -p /root/camera_uart/selftest_RUN001/cam0_nv12
mkdir -p /root/camera_uart/selftest_RUN001/cam1_nv12
mkdir -p /root/camera_uart/selftest_RUN001/cam0_photo
mkdir -p /root/camera_uart/selftest_RUN001/cam1_photo
```

`【电脑端】`

```bash
mkdir -p ~/rk3576_selftest_RUN001
```

### 4.2 记录软件和系统版本

`【板卡 Shell】`

```bash
date -Ins
uname -a
sha256sum /root/camera_uart/camera_aiq_test
sha256sum /etc/iqfiles/cam0/imx586_default_default.json
sha256sum /etc/iqfiles/cam1/imx586_default_default.json
df -h /root
free -m
```

当前目标程序的 SHA-256 应为：

```text
1e8c22dafad98f01ca4ac6c46b0cc3ccaa6f776cfb4ce33b2afb66710daef719
```

若哈希不同，先在报告中记录实际值。不能直接引用本版本以前的测试结论。

### 4.3 避免多进程抢占相机

`【板卡 Shell】`

```bash
systemctl stop camera-uvc.service
pgrep -a camera_aiq_test
pgrep -a rkaiq_3A_server
```

通过条件：两个 `pgrep` 都没有输出。若还有程序，先回到它原来的终端输入 `quit`，
不要同时启动第二个相机程序，也不要另开 `v4l2-ctl` 占用板端采集节点。

---

## 5. 阶段 0：无 MCU 软件自检

本节在开发电脑的源码目录运行，不需要板卡出图。

`【电脑端】`

```bash
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart
make -f Makefile.camera_aiq \
  check-xvs-uart \
  check-control-uart-host \
  check-time-sync \
  check-stage6-host \
  check-stage7-host
```

程序自身还支持以下无硬件测试：

`【板卡 Shell】`

```bash
cd /root/camera_uart
./camera_aiq_test --control-uart-protocol-self-test
./camera_aiq_test --sync-protocol-self-test
./camera_aiq_test --sync-bind-self-test
./camera_aiq_test --photo-exif-self-test
```

通过条件：全部命令退出码为 0，并看到相应的 `*_TEST_OK`。这些结果只能标记为
“软件通过”，不能替代 UART 实线、XVS 波形或真实照片测试。

本手册编写时在开发电脑实跑得到：

```text
XVS_UART_MOCK_TEST_OK one_uart_mux=CAM,XVS_ACK,EVT
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14 pty=115200_8N1_ACK"
TIME_SYNC_SERVICE_TEST_OK state=HOLDOVER utc_valid=0
STAGE6_HOST_TEST_OK
STAGE7_TIME_PIPELINE_TEST_OK
```

`HOLDOVER utc_valid=0` 是无真实 PPS/GNSS 的软件测试结果，不是正式 UTC 锁定。

---

## 6. 阶段 1：双相机、双 ISP 和两份 IQ

### 6.1 检查 media/V4L2 能力

`【板卡 Shell】`

```bash
v4l2-ctl --list-devices
for d in /dev/media*; do echo "===== $d ====="; media-ctl -p -d "$d"; done
v4l2-ctl -d /dev/video22 --list-formats-ext
v4l2-ctl -d /dev/video31 --list-formats-ext
```

若 `/dev/video22` 或 `/dev/video31` 不存在，以 `v4l2-ctl --list-devices` 和后续
`CAMERA_INIT` 输出为准。通过条件：

- 能找到两颗不同 I2C 地址的 IMX586：`m00_b_imx586 4-001a` 和
  `m01_b_imx586 5-001a`。
- 两路 media graph 都是 4000x3000、RAW10 输入。
- 两个 ISP 输出节点都支持 4000x3000 NV12/NV12M。

### 6.2 启动程序

未接 MCU 时使用：

`【板卡 Shell】`

```bash
cd /root/camera_uart
./camera_aiq_test
```

接好 MCU 后使用正式方式：

`【板卡 Shell】`

```bash
cd /root/camera_uart
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

启动日志必须同时出现：

```text
cid[0] ... iq:/etc/iqfiles/cam0/imx586_default_default.json
cid[1] ... iq:/etc/iqfiles/cam1/imx586_default_default.json
CAMERA_INIT camera_id=0 ... state=PREPARED
CAMERA_INIT camera_id=1 ... state=PREPARED
CAMERA_BACKEND_READY cameras=2
```

使用 `--uart` 时还必须看到 `/dev/ttyS9` 为 115200、8N1，且 MCU 的 `PING`、
`IDLE` 均得到正确 ACK。超时或 CRC 错误时不能继续正式同步测试。

### 6.3 启动双路采集

`【camera-aiq>】`

```text
stream-start all
wait 3000
status all
capture-status all
```

未接 MCU 的预期结果：`running=1`，但两路 `frames=0`、`fps_x1000=0`。此时只可
判定初始化和静态配置通过。

接 MCU 后执行：

`【camera-aiq>】`

```text
sync-bind-reset 1
sync-bind-log /root/camera_uart/selftest_RUN001/sync_bind.csv
sync-start 4 10
wait 5000
sync-controller-status
capture-status all
```

通过条件：

- 两路 `running=1`，`frames` 连续增长。
- 两路 `last_errno=0`，`sequence_drops=0`。
- `sync-controller-status` 显示 MCU 正以 4 Hz、低脉宽 10 us 输出。
- cam0/cam1 的画面来源正确，没有串路。

---

## 7. 阶段 2：曝光、增益/ISO 和帧率独立控制

只有 `capture-status all` 的两路 `frames` 正在增长时才执行本节。XVS 从模式没有
图像帧时，RKAIQ 参数无法完成真实回读，只能验证命令路径。

### 7.1 先记录自动曝光基线

`【camera-aiq>】`

```text
auto 0
auto 1
wait 3000
status all
```

抄下两路的 `exposure_us`、`gain_x1000`、`iso`、`fps_x1000` 和 `frames`。

### 7.2 设置两路不同曝光和增益

`【camera-aiq>】`

```text
exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
wait 3000
status all
```

期望：

| 相机 | 请求曝光 | 请求增益 | 期望 ISO（基准 50） |
|---|---:|---:|---:|
| cam0 | 5000 us | 2.000x | 约 100 |
| cam1 | 20000 us | 8.000x | 约 400 |

通过条件：

- cam0 `exposure_us` 约 5000，cam1 约 20000。
- cam0 `gain_x1000` 约 2000，cam1 约 8000。
- 两路 `manual_settings_verified=1`。
- 曝光允许误差为 `max(100 us, 目标值的 2%)`。
- 增益允许误差为 `max(50, 目标值的 2%)`。
- 设置 cam0 时 cam1 不停流、不被改值；设置 cam1 时 cam0 不停流、不被改值。

### 7.3 交叉隔离测试

`【camera-aiq>】`

```text
status all
exposure 0 10000
wait 2000
status all
gain 1 12000
wait 2000
status all
```

判定方法：

1. 第一次 `status all` 是基线。
2. `exposure 0 10000` 后只有 cam0 曝光改变，cam1 的曝光和增益保持原值。
3. `gain 1 12000` 后只有 cam1 增益改变，cam0 的曝光和增益保持原值。
4. 全过程两路 `frames` 都增加，未操作的一路不能停止或重启。

### 7.4 ISO 设置测试

`【camera-aiq>】`

```text
iso 0 150
iso 1 600
wait 3000
status all
```

期望 cam0 约 3.000x/ISO 150，cam1 约 12.000x/ISO 600。若 `aiq_iso=0`，但
`iso`、`gain_x1000` 和请求一致且 `iso_estimated=1`，可判“增益映射 ISO 控制
通过”，不能写成“RKAIQ 原生 ISO 回读通过”。

### 7.5 帧率四种组合

正式独立帧率测试必须保持共享 XVS 为 4 Hz。每组先记 `capture-status all` 中的
`frames`，等待 10 秒再记一次，用后值减前值。

`【camera-aiq>】`

```text
fps 0 4
fps 1 4
capture-status all
wait 10000
capture-status all

fps 0 4
fps 1 2
capture-status all
wait 10000
capture-status all

fps 0 2
fps 1 4
capture-status all
wait 10000
capture-status all

fps 0 2
fps 1 2
capture-status all
wait 10000
capture-status all
```

记录表：

| 组合 | cam0 10 秒帧增量 | cam1 10 秒帧增量 | 期望 | 丢帧 | 结果 |
|---|---:|---:|---|---:|---|
| 4/4 |  |  | 约 40 / 40 |  |  |
| 4/2 |  |  | 约 40 / 20 |  |  |
| 2/4 |  |  | 约 20 / 40 |  |  |
| 2/2 |  |  | 约 20 / 20 |  |  |

允许测试窗口边界相差 1 帧。还必须满足：

- 4 Hz 路 `fps_x1000` 约 4000，2 Hz 路约 2000，`fps_stable=1`。
- `xvs_input_thin=0` 对应 4 Hz，`xvs_input_thin=1` 对应 2 Hz。
- 切换一路时另一条路持续出帧，曝光、增益和目标帧率不被改写。
- `sequence_drops=0`。

不要用 `sync-start 2` 做两路独立 2/4 Hz 验收。全局 XVS 改成 2 Hz 后，thin=1 的
相机会变成约 1 Hz。`sync-start 2` 只用于共享外部 2 Hz 的单独诊断。

当前程序不接受 `sync-start 1`。任务文档中的外部 1 Hz 项应记录为“当前软件不
支持”，不能写成未测通过。

### 7.6 RAW10、YUV、JPEG 和最高帧率边界

- 当前 `camera_aiq_test` 的实际采集源是 ISP 输出的 4000x3000 NV12/NV12M，适合
  验证 YUV、JPEG、UVC、HTTP 和 eMMC 链路。
- RAW10 必须在板卡端找到两路 rkcif RAW 节点，单独用 V4L2 抓帧验证。不能把
  `--list-formats-ext` 中出现 RAW10 当作真实 RAW 帧通过。
- RAW 节点与相机应用可能共享 sensor/media pipeline。RAW 测试前必须退出
  `camera_aiq_test`，由 MCU 独立保持正确 XVS，再由熟悉 media graph 的人员配置并
  抓取两路 RAW；不要让两个程序同时占用同一 pipeline。
- 当前 XVS 程序只控制 2/4 Hz，当前内核也是低频 XVS 从模式。任务文档中的“最大
  FPS”不能用 4 Hz 结果代替。需要单独准备支持目标最高 XVS 频率的 MCU/程序，或
  备份后切换到自由运行测试版本，再测 RAW/YUV 的实际 FPS、MIPI 错误和资源占用。

因此正常低频产品自测可完成 4000x3000 YUV/JPEG 2/4 Hz；RAW10 实流、外部 1 Hz
和 sensor 最大 FPS 必须在最终表中分别记录，缺少对应测试条件时标为环境阻塞或
不支持，不能合并到“双相机通过”。

---

## 8. 阶段 3：UART 实线控制

### 8.1 UART 物理层

MCU 端设置为 115200、8 数据位、无校验、1 停止位、无软硬件流控。先验证：

- RK3576 TX -> MCU RX 能收到完整 ASCII 行。
- MCU TX -> RK3576 RX 能收到完整 ASCII 行。
- 电平和空闲电平正确，无乱码、丢字节或粘包。

### 8.2 相机控制协议

请求格式：

```text
$CAM,SEQUENCE,CAMERA_ID,COMMAND[,ARGUMENT...]\r\n
```

`camera_id` 0/1 表示指定相机，255 表示全局。MCU 按顺序发送以下命令，每条都要
记录请求、ACK/NACK、耗时和相机实际状态：

```text
$CAM,1,255,PING
$CAM,2,255,GET_STATUS
$CAM,3,255,STREAM_START
$CAM,4,0,EXPOSURE,5000
$CAM,5,1,EXPOSURE,20000
$CAM,6,0,GAIN,2000
$CAM,7,1,GAIN,8000
$CAM,8,0,FPS,4
$CAM,9,1,FPS,2
$CAM,10,0,CAPTURE_STATUS
$CAM,11,1,CAPTURE_STATUS
$CAM,12,0,NET_START
$CAM,13,1,NET_START
$CAM,14,255,UVC_START
$CAM,15,0,SAVE_START,/root/camera_uart/selftest_RUN001/cam0_nv12
$CAM,16,1,SAVE_START,/root/camera_uart/selftest_RUN001/cam1_nv12
```

成功条件：返回相同的 `SEQUENCE` 和 `CAMERA_ID`，并且实际状态与 ACK 内容一致。
无回复、`$NACK`、序号错配、控制到错误相机或只返回 OK 但实际值没改变均为失败。

当前 `$CAM` 协议支持 `GAIN`，本地交互界面支持 `iso`，但
`camera_control_uart.cpp` 尚未把 `$CAM,...,ISO,...` 映射为本地 `iso` 命令。如果
交付协议明确要求 MCU 直接发送 ISO，而不是发送等效增益，这一项仍需补代码后复测。

### 8.3 XVS 控制协议

XVS 命令使用带 CRC16-CCITT-FALSE 的 `$XVS` 帧。交互界面的下列命令会通过同一
`/dev/ttyS9` 自动完成协议和 ACK 匹配：

`【camera-aiq>】`

```text
sync-idle
sync-controller-status
sync-start 4 10
wait 3000
sync-controller-status
sync-stop
sync-controller-status
```

通过条件：MCU 状态依次为 IDLE、RUNNING、IDLE，频率、脉宽、pulse_count 和
last_trigger_id 正确，CRC 错误计数和超时均为 0。

---

## 9. 阶段 4：XVS、帧计数和硬件同步

### 9.1 示波器连接

建议四通道同时观察：

| 通道 | 测点 | 检查内容 |
|---|---|---|
| CH1 | MCU 的 XVS 输出脚 | 源周期和低脉宽 |
| CH2 | RK3576 板上 `FSYNC_CAM`/缓冲前后测试点 | 板级输入是否正确 |
| CH3 | cam0 IMX586 pin 26 附近可测点 | cam0 收到的 XVS |
| CH4 | cam1 IMX586 pin 26 附近可测点 | cam1 收到的 XVS |

示波器地线必须与板卡地共地，探头不要造成 XVS 过大负载。

### 9.2 波形判定

共享 4 Hz 时记录：

| 指标 | 实测值 | 期望 | 结果 |
|---|---:|---:|---|
| 周期 |  | 250000 us |  |
| 频率 |  | 4 Hz |  |
| 低脉宽 |  | 10 us |  |
| 空闲电平 |  | 高 |  |
| cam0/cam1 XVS 到达边沿差 |  | 小于产品同步预算 |  |
| 连续 1000 脉冲异常数 |  | 0 |  |

仅看到两颗 sensor 输入脚上的 XVS 边沿一致，只能证明脉冲分发一致，不能单独证明
两颗 sensor 的实际曝光起点完全一致。完整结论还要结合从模式寄存器、帧计数、帧
时间戳，以及可测的 sensor SOF/曝光指示信号。

### 9.3 1000 脉冲与帧计数

IMX586 从模式的第一个脉冲可能用于 pre-shutter，因此先发一个预备脉冲：

`【camera-aiq>】`

```text
stream-start all
sync-start 4 10
fps 0 4
fps 1 4
sync-idle
sync-count 4 1 10
wait 1000
capture-status all
sync-count 4 1000 10
wait 251000
sync-controller-status
capture-status all
sync-bind-status
```

4/4 组合下，两路应各增加约 1000 帧。再测试 4/2 和 2/4：

- 4 Hz 路约增加 1000 帧。
- 2 Hz 路约增加 500 帧。
- 两路 `sequence_drops=0`。
- MCU `PULSE_COUNT` 增量准确，停止后帧数不再增长。

如要让 2 Hz 路也验收 1000 帧，应保持共享 XVS 为 4 Hz，并发送约 2000 个脉冲。

### 9.4 同步结论边界

`sync-status` 的 `delta_ns` 来自 ISP/V4L2 buffer 时间，只能辅助判断两路帧是否
配对，不能当作传感器曝光起点误差。正式同步报告至少同时包含：

1. 示波器的 XVS 周期、脉宽和两路到达边沿差。
2. MCU trigger_id 和 pulse_count。
3. 两路 frame_id/sequence 增量及丢帧数。
4. `sync_bind.csv` 中左右帧绑定关系。
5. 若产品规定曝光起点误差上限，还需可测的 SOF/曝光起始硬件信号或 sensor 级硬件时间戳。

---

## 10. 阶段 5：PPS、GPRMC、Trigger 和 frame_id

### 10.1 时间源锁定

MCU 依次上报 PPS、GPRMC/GNRMC 和 XVS 事件。`PPS.timer_tick` 与
`XVS.timer_tick` 必须来自同一计数器。

`【camera-aiq>】`

```text
sync-idle
time-sync-reset
sync-bind-reset 1
sync-bind-log /root/camera_uart/selftest_RUN001/sync_bind.csv
sync-start 4 10
wait 5000
time-sync-status
sync-bind-status
sync-bind-last
```

正式通过必须看到：

```text
state=UTC_LOCKED
utc_valid=1
```

还要满足 PPS、RMC/NMEA、XVS 计数持续增长，`pps_id` 和 `trigger_id` 单调递增，
CRC 错误、无效 NMEA、时间回退和绑定丢弃均为 0。

未接 MCU 时可运行 `sync-sim-start` 和自检验证逻辑，但 `utc_valid=0`，只能写成
“系统时间/模拟逻辑通过”，不能写成 UTC 授时通过。

### 10.2 帧绑定核对

打开板卡另一个 Shell：

`【板卡 Shell】`

```bash
head -n 5 /root/camera_uart/selftest_RUN001/sync_bind.csv
tail -n 10 /root/camera_uart/selftest_RUN001/sync_bind.csv
```

逐行核对：

- `trigger_id` 单调递增且没有重复。
- 同一 trigger 下 cam0/cam1 的 `camera_id` 正确。
- `frame_id`/V4L2 sequence 单调递增。
- 4/4 时每个有效 trigger 都应形成左右帧；4/2 时必须按设计说明记录被 thinning 的 trigger。
- 不能用应用收到 UART 报文的时间替代 MCU 在 PPS/XVS 边沿锁存的 `timer_tick`。

---

## 11. 阶段 6：eMMC、JPEG、EXIF 和照片信息

### 11.1 eMMC/NV12 短时保存

4000x3000 NV12 单帧大小为：

```text
4000 * 3000 * 3 / 2 = 18000000 bytes
```

先确认空间。4 Hz + 2 Hz 两路原始 NV12 合计约 108 MB/s，不要直接做 60 分钟
原始保存，否则需要约 389 GB 空间。

`【板卡 Shell】`

```bash
lsblk -o NAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS
findmnt -T /root
df -h /root
```

先确认测试目录实际位于 eMMC。若 `/root` 不在 eMMC，应把后续输出目录替换成
真实 eMMC 挂载点；否则这只能算文件保存测试，不能算 eMMC 保存通过。

`【camera-aiq>】`

```text
save-start 0 /root/camera_uart/selftest_RUN001/cam0_nv12
save-start 1 /root/camera_uart/selftest_RUN001/cam1_nv12
wait 10000
save-stop 0
save-stop 1
capture-status all
```

`【板卡 Shell】`

```bash
find /root/camera_uart/selftest_RUN001/cam0_nv12 -name '*.nv12' -type f | wc -l
find /root/camera_uart/selftest_RUN001/cam1_nv12 -name '*.nv12' -type f | wc -l
find /root/camera_uart/selftest_RUN001 -name '*.nv12' -type f -printf '%s\n' | sort -u
wc -l /root/camera_uart/selftest_RUN001/cam0_nv12/cam0_frames.csv
wc -l /root/camera_uart/selftest_RUN001/cam1_nv12/cam1_frames.csv
```

通过条件：

- 每个 NV12 都是 18000000 字节。
- 4 Hz 路 10 秒约 40 个文件，2 Hz 路约 20 个文件。
- CSV 数据行数和文件数一致。
- `save_queue_drops=0`、`save_failures=0`、`last_errno=0`。

### 11.2 JPEG、frame_id 和 EXIF

为减少 2 Hz thinning 对 trigger 配对判断的干扰，照片元数据正式验收先把两路都
设为 4 Hz。`photo-offset` 必须填写示波器或标定得到的每路 sensor response offset；
下面的 0 只用于功能联调，不代表曝光起点精度已校准。

`【camera-aiq>】`

```text
fps 0 4
fps 1 4
sync-idle
photo-offset 0 0
photo-offset 1 0
sync-bind-reset 1
sync-bind-log /root/camera_uart/selftest_RUN001/photo_bind.csv
photo-start 0 /root/camera_uart/selftest_RUN001/cam0_photo
photo-start 1 /root/camera_uart/selftest_RUN001/cam1_photo
sync-count 4 41 10
wait 12000
photo-stop 0
photo-stop 1
photo-status all
sync-bind-last
```

`【板卡 Shell】`

```bash
find /root/camera_uart/selftest_RUN001/cam0_photo -name '*.jpg' -type f | wc -l
find /root/camera_uart/selftest_RUN001/cam1_photo -name '*.jpg' -type f | wc -l
find /root/camera_uart/selftest_RUN001/cam0_photo -name '*.jpg' -type f | head -n 1
find /root/camera_uart/selftest_RUN001/cam1_photo -name '*.jpg' -type f | head -n 1
```

把上面找到的实际 JPEG 路径代入：

```bash
exiftool /root/camera_uart/selftest_RUN001/cam0_photo/实际文件名.jpg
exiftool /root/camera_uart/selftest_RUN001/cam1_photo/实际文件名.jpg
```

逐张或抽样核对：

- JPEG 可解码且分辨率为 4000x3000。
- `camera_id` 与目录、画面来源一致。
- `frame_id` 等于对应 V4L2 sequence。
- 左右照片的 `trigger_id` 按设计配对。
- `ExposureTime` 与该帧曝光值一致。
- `PhotographicSensitivity` 与 ISO/gain 映射一致，并记录是否为估算。
- `DateTimeOriginal`/`SubSecTimeOriginal` 对应有效 UTC。
- UserComment 中有 trigger、PPS、frame、exposure start/center 和增益信息。
- `encode_errors=0`、`exif_errors=0`、`write_errors=0`。

当前实现中的曝光参数来自 DQBUF 时的 RKAIQ 最新查询。固定手动曝光时可用于功能
验收；自动曝光或快速切换参数时，若没有 sensor 每帧寄存器快照，不能声称曝光值
已经严格绑定到该 frame_id。

---

## 12. 阶段 7：USB 3.0、双 UVC、网口 HTTP 和并发输出

### 12.1 启动双 UVC 和双 HTTP

确保双路采集已经启动，并重新启动连续 4 Hz XVS。上一节的 `sync-count` 完成后
MCU 会回到 IDLE，不执行这里的 `sync-start` 就不会继续产生图像帧。

`【camera-aiq>】`

```text
stream-start all
sync-start 4 10
uvc-start all
net-start 0
net-start 1
wait 3000
uvc-status all
net-status all
capture-status all
```

### 12.2 板卡检查 USB 复合设备

`【板卡 Shell】`

```bash
cat /sys/class/udc/23000000.usb/state
cat /sys/class/udc/23000000.usb/current_speed
find /sys/kernel/config/usb_gadget -path '*/configs/*' -type l -printf '%f -> %l\n'
ip -brief address show usb0
```

通过条件：

- UDC 为 `configured`。
- 配置中同时存在 `rndis.0`、`uvc.0`、`uvc.1`。
- USB 3.0 正式验收必须为 `super-speed`，`high-speed` 只表示 USB 2.0 480 Mbps。
- `usb0` 地址为 `192.168.55.1/24` 或产品配置的固定地址。

### 12.3 电脑识别 USB 速率和网口

`【电脑端】`

```bash
lsusb
lsusb -t
ip -brief address
ping -c 10 192.168.55.1
```

通过条件：

- RK3576 复合设备枚举成功。
- `lsusb -t` 对应设备显示 `5000M` 或更高才算 USB 3.x；`480M` 不通过 USB 3.0。
- 电脑 RNDIS/USB 网卡获得 `192.168.55.x/24`。
- ping 10 包无丢包。

若只显示 480M，应把板卡 Type-C 数据口用具备 SuperSpeed 数据对的线直接连接电脑
USB 3.x 端口，不要经过只有 USB 2.0 的 Hub。修改 `maximum_speed` 不能把缺少
SuperSpeed 物理链路的连接变成 USB 3.0。

### 12.4 双 HTTP 图像测试

不需要 SSH 端口转发，电脑直接访问：

```text
http://192.168.55.1:8080/
http://192.168.55.1:8080/cam0
http://192.168.55.1:8080/cam1
```

`【电脑端】`

```bash
curl -sS --max-time 5 -o /tmp/rk3576_http_index.html \
  -w 'HTTP=%{http_code} bytes=%{size_download}\n' \
  http://192.168.55.1:8080/
ffmpeg -y -loglevel error -i http://192.168.55.1:8080/cam0 \
  -frames:v 1 ~/rk3576_selftest_RUN001/http_cam0.jpg
ffmpeg -y -loglevel error -i http://192.168.55.1:8080/cam1 \
  -frames:v 1 ~/rk3576_selftest_RUN001/http_cam1.jpg
file ~/rk3576_selftest_RUN001/http_cam0.jpg
file ~/rk3576_selftest_RUN001/http_cam1.jpg
```

通过条件：两个 URL 都有持续 MJPEG 数据，两张图都是 4000x3000 JPEG 且来源正确，
`net-status all` 中 `encode_errors=0`、`http_errors=0`。只返回 HTTP 200 响应头但
文件为 0 字节时，必须检查 `capture-status all`；源 `frames=0` 时不能判 HTTP
视频通过。

### 12.5 找出电脑上的两个 UVC 图像节点

`【电脑端】`

```bash
v4l2-ctl --list-devices
```

对 RK3576 下的每个 `/dev/videoN` 执行：

```bash
v4l2-ctl -d /dev/videoN --list-formats-ext
```

支持 `MJPG 4000x3000` 的两个节点是图像节点；另外两个可能是 metadata 节点。
重新插拔后编号会变化，不能永久假定为 `/dev/video0` 和 `/dev/video2`。下文用
`/dev/videoX` 表示 cam0、`/dev/videoY` 表示 cam1，测试时替换成实际节点。

### 12.6 两路分别抓图

`【电脑端】`

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=1 \
  --stream-to=$HOME/rk3576_selftest_RUN001/uvc_cam0.jpg

v4l2-ctl -d /dev/videoY \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=2 --stream-mmap=4 --stream-count=1 \
  --stream-to=$HOME/rk3576_selftest_RUN001/uvc_cam1.jpg

file ~/rk3576_selftest_RUN001/uvc_cam0.jpg
file ~/rk3576_selftest_RUN001/uvc_cam1.jpg
```

通过条件：两张 JPEG 均为 4000x3000、可打开、画面来源不串路。

### 12.7 双 UVC 同时连续取流

先在 `camera-aiq>` 设置 cam0=4 Hz、cam1=2 Hz，然后在电脑端同时执行 10 秒测试：

`【电脑端】`

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=40 --stream-to=/dev/null &
PID0=$!

v4l2-ctl -d /dev/videoY \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=2 --stream-mmap=4 --stream-count=20 --stream-to=/dev/null &
PID1=$!

wait "$PID0"
wait "$PID1"
```

随后在 `【camera-aiq>】` 执行：

```text
uvc-status all
capture-status all
net-status all
```

通过条件：两条电脑端命令约 10 秒完成，板端两路 `submitted/encoded/sent` 增长，
编码错误和发送错误为 0，HTTP 同时仍可访问。若 `host_streaming=1` 但
`submitted=0`，先检查源采集 `frames` 是否为 0。

### 12.8 UVC、网口和 eMMC 同时工作

保持双 UVC 和双 HTTP 客户端取流，再执行 10 秒 NV12 保存。通过条件：

- 两路 UVC 连续。
- 两路 HTTP 连续。
- 两路 NV12 文件数和 4/2 Hz 对应。
- `sequence_drops=0`、`save_queue_drops=0`、`save_failures=0`。
- `encode_errors=0`、`http_errors=0`。
- 程序不崩溃、不重启，相机参数不串路。

YOLO 不属于当前 `camera_aiq_test` 的命令。若交付整机还包括 YOLO，应启动实际
YOLO 程序和模型后单独增加 CPU/NPU/DDR、结果正确性和并发稳定性记录，不能仅凭
相机程序进入运行态判 YOLO 通过。

### 12.9 按 camera_id 独立停止和恢复输出

先让电脑持续打开 cam1 的 UVC 和 HTTP，再只停止 cam0：

`【camera-aiq>】`

```text
uvc-stop 0
net-stop 0
wait 3000
uvc-status all
net-status all
capture-status all
```

通过条件：cam0 UVC/HTTP 停止，cam1 的 UVC/HTTP 和源采集持续增长，USB 不重新
枚举，cam1 不断流。然后恢复 cam0：

```text
uvc-start 0
net-start 0
wait 3000
uvc-status all
net-status all
```

交换 camera_id 再做一次：

```text
uvc-stop 1
net-stop 1
wait 3000
uvc-status all
net-status all
capture-status all
uvc-start 1
net-start 1
```

两次测试都必须在电脑端实际观察未停止的一路连续收到图像，不能只看板端
`enabled=1`。

当前版本已将 UVC 视频生产与复合 Gadget/RNDIS 生命周期解耦。执行
`uvc-stop all` 只停止两路 UVC 编码和送帧，程序应返回
`usb_gadget=kept rndis=kept`；UVC 控制端点、RNDIS 网卡和已经启动的 HTTP 服务
必须继续存在。随后执行 `uvc-start all` 应直接恢复，不应发生 USB 重新枚举。

按以下步骤专门验收网口连续性。电脑端先持续执行：

`【电脑终端】`

```bash
ping -i 0.2 192.168.55.1
```

板端程序内执行：

`【camera-aiq>】`

```text
uvc-status all
net-status all
uvc-stop all
wait 3000
uvc-status all
net-status all
uvc-start all
wait 3000
uvc-status all
net-status all
```

另一个板端终端同时检查：

`【板端 shell】`

```bash
systemctl is-active usbdevice.service
cat /sys/kernel/config/usb_gadget/rockchip/UDC
ls -l /sys/kernel/config/usb_gadget/rockchip/configs/b.1/
ip -br address show usb0
```

通过条件：电脑 ping 连续且不重新获得网卡地址；板端 UDC 始终有控制器名称；
`f-rndis.0`、`f-uvc.0`、`f-uvc.1` 链接始终存在；两路 HTTP 的 `server_running=1`；
恢复后两路 UVC 再次为 `enabled=1`。程序退出后 RNDIS 仍由 `usbdevice.service`
维护，但 HTTP 属于相机程序，会随程序退出；若要求 HTTP 也在相机程序退出后存在，
应把 HTTP 后端另做常驻服务。正常相机控制不得调用 `usbdevice stop/restart`，也不得
手工解绑 UDC，因为这两类操作会让整个复合 USB 和 RNDIS 一起掉线。

---

## 13. 30～60 分钟稳定性、资源和功耗

### 13.1 长稳模式

长稳测试不要持续保存原始 NV12，以免磁盘先被写满。建议持续打开：

- 双路采集，cam0=4 Hz、cam1=2 Hz。
- 双 UVC 电脑端取流到 `/dev/null`。
- 双 HTTP 客户端取流到 `/dev/null`。
- 每 10 分钟短时保存一次 NV12 和 JPEG 作完整性抽查。

至少运行 30 分钟，正式交付建议 60 分钟。

### 13.2 每 10 分钟记录一次

`【camera-aiq>】`

```text
status all
capture-status all
uvc-status all
net-status all
photo-status all
sync-controller-status
time-sync-status
sync-bind-status
```

`【板卡 Shell】`

```bash
date -Ins
uptime
free -m
top -b -n 1 | head -n 20
for f in /sys/class/thermal/thermal_zone*/temp; do echo "$f $(cat "$f")"; done
dmesg | grep -Ei 'imx586|mipi|csi|isp|rkcif|error|timeout' | tail -n 100
```

记录：总帧数、10 分钟帧增量、实际 FPS、所有 drop/error 计数、CPU、RSS、可用
内存、各温区、USB 速率、网络断连次数和服务重启次数。

### 13.3 功耗

用板卡输入端的外置功率计分别记录：

| 工况 | 电压 | 平均电流 | 平均功率 | 峰值功率 |
|---|---:|---:|---:|---:|
| 待机 |  |  |  |  |
| 双采集 4/2 Hz |  |  |  |  |
| 双 UVC |  |  |  |  |
| 双 HTTP |  |  |  |  |
| UVC + HTTP + eMMC |  |  |  |  |
| 加 YOLO 的整机满功能 |  |  |  |  |

无外置功率计时，本项只能标“未测试”，不能用 CPU 占用率估算成通过。

---

## 14. 测试结束和恢复服务

先停止保存和照片，再停止网络、同步和采集。

`【camera-aiq>】`

```text
save-stop 0
save-stop 1
photo-stop 0
photo-stop 1
net-stop all
sync-stop
stream-stop all
quit
```

某项本来未启动而返回状态错误不影响清理。程序退出后恢复开机服务：

`【板卡 Shell】`

```bash
systemctl start camera-uvc.service
systemctl status camera-uvc.service --no-pager
```

未连接 MCU 时，若服务参数包含 `--uart /dev/ttyS9` 并因 MCU 超时启动失败，应保持
服务停止并在测试记录中注明，不要反复重启制造无效日志。

---

## 15. 常见失败和定位顺序

| 现象 | 先检查 | 原因和处理 |
|---|---|---|
| `camera is not ready` | `capture-status all` | RKAIQ 只 PREPARED、没有开始出流；先 `stream-start`，XVS 从模式还必须有外部脉冲 |
| `running=1` 但 `frames=0` | 示波器、`sync-controller-status` | 没有 XVS、XVS 电平/极性错误、MCU 未运行或 sensor 从模式配置错误 |
| HTTP 200 但没有图片 | `capture-status all`、`net-status all` | 200 只代表服务器接受连接；源帧为 0 时没有 MJPEG payload |
| UVC `host_streaming=1` 但 `submitted=0` | `capture-status all` | 电脑已协商端点，但板端没有源帧 |
| 浏览器打不开 `127.0.0.1:18080` | 电脑 `ip -brief address` | `127.0.0.1` 是电脑自己；RNDIS 直连应访问 `http://192.168.55.1:8080/` |
| USB 只有 480M | 板端 `current_speed`、电脑 `lsusb -t` | 当前经过 USB2 Hub、线没有 SuperSpeed 数据对或电脑端口不是 USB3；改为直连 |
| 修改 FPS 返回 OK 但帧数不变 | 10 秒前后 `frames` 差值 | 不能只看 ACK；检查共享 XVS 是否 4 Hz、`xvs_input_thin` 回读和 sensor 寄存器 |
| 曝光/增益回读不对 | `manual_settings_verified`、`last_aiq_error` | 确认正在出帧，等待 2～3 秒；按误差范围核对，检查 RKAIQ 查询和 sensor 生效 |
| `aiq_iso=0` | `iso`、`iso_estimated`、各级增益 | 当前可验证估算 ISO/增益映射；原生 AIQ ISO 仍需单独修复或明确协议边界 |
| 一路参数影响另一路 | 启动日志的 sensor/IQ/context | 检查 camera_id 到 sensor 的映射和两套 RKAIQ context，逐条重复隔离测试 |
| 保存文件少 | `save_queue_drops`、磁盘空间 | eMMC 写入不足或队列溢出；检查 `df -h` 和写盘性能 |
| 双 UVC 只一路有画面 | 两路 `frames`、`uvc-status`、电脑节点格式 | 先区分源帧、编码器和 USB 端点；metadata 节点不能当图像节点 |
| UTC 无效 | `time-sync-status` | 必须同时有有效 PPS 和对应 GPRMC/GNRMC；系统时间不等于 GNSS UTC 锁定 |
| 帧绑定丢失 | `$EVT,XVS`、trigger_id、pre-shutter 设置 | 检查 MCU 边沿锁存、UART 事件完整性、`sync-bind-reset 1` 和两路出帧 |
| 程序启动卡住 | 串口和 XVS ACK | `--uart` 启动会等待 MCU PING/IDLE；无 MCU 时用不带 `--uart` 的交互方式 |

定位必须按以下顺序，避免在没有源帧时反复修改输出端：

```text
MCU/XVS 波形
  -> sensor 是否出帧
  -> capture-status 的 frames
  -> 参数和 frame 绑定
  -> MPP JPEG 编码
  -> UVC/HTTP/eMMC 输出
  -> 电脑端显示
```

---

## 16. 最终测试记录表

| 序号 | 测试项 | 必须证据 | 本次结果 | 备注/文件路径 |
|---:|---|---|---|---|
| 1 | 两颗 IMX586 和双 media graph | media/V4L2 日志 |  |  |
| 2 | 两份 IQ 分别加载 | 两条 AIQ init 路径 |  |  |
| 3 | 双路真实采集 | 两路 frames 增长 |  |  |
| 4 | 曝光独立 | 请求值、回读值、隔离结果 |  |  |
| 5 | 增益独立 | 请求值、回读值、隔离结果 |  |  |
| 6 | ISO 设置/来源说明 | iso、aiq_iso、iso_estimated |  |  |
| 7 | 4/4 Hz | 10 秒帧增量 |  |  |
| 8 | 4/2 Hz | 10 秒帧增量 |  |  |
| 9 | 2/4 Hz | 10 秒帧增量 |  |  |
| 10 | 2/2 Hz | 10 秒帧增量 |  |  |
| 11 | 外部 1/2/4 Hz | 波形和帧计数 |  |  |
| 12 | UART 115200 8N1 实线 | 请求/ACK 日志 |  |  |
| 13 | XVS 1000 脉冲 | 示波器、pulse/frame 计数 |  |  |
| 14 | PPS + GPRMC/GNRMC | UTC_LOCKED、utc_valid=1 |  |  |
| 15 | trigger/frame_id 绑定 | sync_bind.csv |  |  |
| 16 | JPEG/EXIF/UTC | JPEG、exiftool、CSV |  |  |
| 17 | eMMC NV12 | 文件、CSV、drop/error=0 |  |  |
| 18 | USB 3.0 | current_speed、lsusb -t |  |  |
| 19 | 双 UVC 实流 | 双路连续取帧日志和图片 |  |  |
| 20 | 双 HTTP 实流 | 两路 URL、图片和状态 |  |  |
| 21 | UVC + RNDIS + eMMC 并发 | 全部计数增长、错误为 0 |  |  |
| 22 | 30～60 分钟长稳 | 周期状态、温度、错误日志 |  |  |
| 23 | 整机功耗 | 外置功率计记录 |  |  |
| 24 | 双路 RAW10 实流 | 两路 RAW 帧计数和有效数据 |  |  |
| 25 | Sensor 最大 FPS | 实际帧率、MIPI/资源/温度 |  |  |
| 26 | 停止/恢复 UVC 时 RNDIS 连续在线 | 连续 ping、UDC、configfs、uvc/net-status |  |  |

---

## 17. 最终放行条件

只有同时满足以下条件，才能把 v0.2 写成“整机自测通过”：

1. 两路在共享 4 Hz XVS 下可独立切换 4/2、2/4、4/4、2/2，实际帧计数正确且零丢帧。
2. 曝光、增益/ISO 和帧率按 camera_id 独立设置、独立回读，另一条路持续工作且参数不变。
3. UART 实线 115200 8N1 的命令、ACK、CRC、超时和异常恢复全部通过。
4. 示波器证明 XVS 频率、脉宽和两路到达边沿满足产品同步预算，1000 脉冲帧计数正确。
5. PPS/GPRMC 达到 `UTC_LOCKED`，trigger、frame_id、曝光时间、增益和照片元数据一致。
6. 双 UVC、双 HTTP 和双 eMMC 均有真实图像数据，并发运行没有 drop/error。
7. 板端和电脑端都证明 USB 实际协商为 SuperSpeed，而不是 USB2 480M。
8. 30～60 分钟长稳无崩溃、无服务重启、无持续 MIPI/ISP 错误、温度和资源合格。
9. 满功能功耗有实际仪器数据，并满足整机指标。

当前未接 MCU 时，最多可完成软件自检、静态相机配置、RNDIS/HTTP 控制面、双 UVC
枚举和 USB 速率检查。真实 XVS 出帧、硬件同步、UTC、实拍输出和满负载长稳必须在
MCU 接入后按路径 B 重新执行，不能沿用模拟测试代替。
