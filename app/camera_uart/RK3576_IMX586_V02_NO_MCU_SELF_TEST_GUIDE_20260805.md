# RK3576 双 IMX586 v0.2 无 MCU 自测手册

文档版本：V1.3  
日期：2026-08-05  
适用硬件：RK3576、两路 IMX586、Ubuntu 测试电脑  
适用程序：`/root/camera_uart/camera_aiq_test`  
测试依据：《RK3576 MIPI 相机模组任务计划 v0.2》阶段 1～7及第四部分测试表

---

## 1. 先理解当前测试边界

当前板卡运行的是**自由运行设备树**，没有启用 `sony,xvs-slave-mode`。因此未接 MCU
时两路相机仍会连续出图，实测约 23～25 fps。这个版本可以真实验证双摄、独立参数、
JPEG、eMMC、双 UVC、双 HTTP、RNDIS和 SSH。

但以下项目不能靠自由运行画面判定通过：

- sensor 底层真实 2 Hz/4 Hz；
- 两颗 IMX586 的 XVS硬件同步；
- MCU和 RK3576之间的 UART实线；
- PPS/GPRMC提供的真实 UTC；
- 示波器测得的 XVS相位、曝光起点和同步误差；
- 功耗仪读数。

程序的 UART、Trigger、PPS/UTC、frame_id和 EXIF链路均有模拟测试。模拟通过必须写成
“软件通过”，不能写成“硬件通过”。

### 1.1 本手册替代哪一部分旧说明

`RK3576_IMX586_V02_SELF_TEST_GUIDE_20260804.md` 按早期 XVS从模式版本编写，其中
“未接 MCU时 frames=0”的描述不适用于当前板卡。本手册是当前自由运行版本的执行依据。

### 1.2 结果只使用四种结论

| 结论 | 含义 |
|---|---|
| 实机通过 | 有本次真实帧、真实文件或真实链路证据，且数值满足要求 |
| 软件通过 | 模拟器、伪终端或程序自检通过，外部硬件尚未验收 |
| 待硬件验收 | 缺 MCU、XVS、PPS/GNSS、示波器或功耗仪 |
| 不通过 | 当前具备测试条件，但实测不满足需求 |

---

## 2. 终端、设备和参数

| 名称 | 当前设备/地址 | 用途 |
|---|---|---|
| 电脑调试串口 | `/dev/ttyUSB0`，1500000 | 登录板卡 Shell |
| MCU控制串口 | 板端 `/dev/ttyS9`，115200、8N1 | 以后连接 MCU，不是调试串口 |
| cam0 ISP | `/dev/video22` | 板端 4000x3000 NV12M |
| cam1 ISP | `/dev/video31` | 板端 4000x3000 NV12M |
| cam0 RAW | `/dev/video0` | 板端 RAW10 |
| cam1 RAW | `/dev/video11` | 板端 RAW10 |
| USB网口 | 板卡 `192.168.55.1` | ping、SSH、HTTP |
| HTTP cam0 | `http://192.168.55.1:8080/cam0` | MJPEG流 |
| HTTP cam1 | `http://192.168.55.1:8080/cam1` | MJPEG流 |

设备编号可能随内核和 media graph变化。每次都先看 `CAMERA_INIT`、
`capture-status all` 和电脑端 `v4l2-ctl --list-devices`，不要盲目照抄节点。

参数单位：

| 命令 | 含义 |
|---|---|
| `exposure 0 5000` | cam0曝光 5000 us |
| `gain 0 2000` | cam0模拟增益 2.000倍 |
| `iso 0 150` | cam0 ISO控制入口，当前按基础 ISO 50映射为3倍增益 |
| `fps 0 4` | 请求 cam0 sensor输出4 Hz，不是只限制 UVC发送速度 |

`gain` 和 `iso` 控制同一个增益目标，后执行者覆盖前执行者。`aiq_iso=0` 表示 RKAIQ
没有返回原生 ISO；`iso_estimated=1` 表示程序按基础 ISO和总增益换算 ISO。

---

## 3. 测试前检查

在板卡 Shell记录版本：

```bash
date -Ins
uname -a
sha256sum /root/camera_uart/camera_aiq_test
sha256sum /etc/iqfiles/cam0/imx586_default_default.json
sha256sum /etc/iqfiles/cam1/imx586_default_default.json
df -h /root
free -m
```

本轮程序 SHA-256：

```text
c6cdd3064a1509ad2175590d2e2e68fe3016758fac0d656f296d0bb26c282a58
```

两份 IQ文件本轮 SHA-256相同：

```text
d0f4a039190140e17995441f72dd162c0157e85ddea808281d3010cb508bb027
```

这表示两路加载路径和 RKAIQ context独立，但两份 IQ内容还没有形成不同镜头的独立标定。

---

## 4. 先跑无 MCU软件自检

电脑端进入源码目录：

```bash
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart
make -f Makefile.camera_aiq \
  check-xvs-uart \
  check-control-uart-host \
  check-time-sync \
  check-stage6-host \
  check-stage7-host
```

五个目标应分别出现：

```text
XVS_UART_MOCK_TEST_OK
CONTROL_UART_PROTOCOL_SELF_TEST_OK
TIME_SYNC_SERVICE_TEST_OK state=HOLDOVER utc_valid=0
STAGE6_HOST_TEST_OK
STAGE7_TIME_PIPELINE_TEST_OK
```

板端程序自检：

```bash
cd /root/camera_uart
./camera_aiq_test --control-uart-protocol-self-test
./camera_aiq_test --sync-protocol-self-test
./camera_aiq_test --sync-bind-self-test
./camera_aiq_test --photo-exif-self-test
```

全部退出码为0才是软件通过。`utc_valid=0` 是没有真实 PPS/GNSS的正确状态。

---

## 5. 启动交互测试程序

必须从调试串口操作，因为停止常驻服务会暂时撤下复合 USB，RNDIS SSH可能断开。

```bash
systemctl stop camera-uvc.service
pgrep -a camera_aiq_test
pgrep -a rkaiq_3A_server
cd /root/camera_uart
./camera_aiq_test
```

只允许一个相机管理进程。看到两个 `CAMERA_INIT` 和 `CAMERA_BACKEND_READY` 后继续。

---

## 6. 阶段 1：双路真实出图

在 `camera-aiq>` 中执行：

```text
stream-start all
wait 5000
status all
capture-status all
wait 5000
capture-status all
```

通过条件：

- 两路 `running=1`；
- 两次查询之间两路 `frames` 都增长；
- `timestamp_valid=1`；
- 节点分别属于 cam0和 cam1；
- 两路真实图像视角不同。

当前自由运行版本约 23～25 fps，不代表需求中的2/4 Hz已经通过。

本轮实测：cam0在约5.8秒内取得150帧，约25.851 fps；cam1在约5.7秒内取得127帧，
约22.157 fps。两路均为4000x3000 NV12M，`running=1`、`timestamp_valid=1`，所以双路
真实出图通过；但 `sequence_drops` 分别为22和44，自由运行高输入率不能填写“零丢帧”。

---

## 7. 阶段 2：曝光、增益、ISO独立性

```text
exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
wait 3000
status all

exposure 0 10000
wait 2000
status all

gain 1 4000
wait 2000
status all

iso 0 150
iso 1 500
wait 3000
status all
```

通过条件：

- 第一次回读 cam0约5000 us/2倍，cam1约20000 us/8倍；
- 只改 cam0曝光时，cam1曝光不变；
- 只改 cam1增益时，cam0增益不变；
- ISO 150/500分别映射为约3倍/10倍增益；
- 曝光误差不大于2%或100 us，增益误差不大于2%或50。

本轮回读为 cam0 5004 us、2倍、ISO 100，cam1 19996 us、8倍、ISO 400；随后只把
cam0设为ISO 200时，cam0变为4倍/ISO 200，cam1仍保持8倍/ISO 400。独立控制、回读
和左右隔离实机通过。这里的ISO是程序按基础ISO 50与总增益换算的值，`aiq_iso=0`、
`iso_estimated=1`，不能写成“RKAIQ原生ISO回读通过”。

---

## 8. 阶段 3：严格测试 sensor 2/4 Hz

四组必须全部测，且每组设置后记录10秒帧增量：

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

期望增量：`4/4=40/40`、`4/2=40/20`、`2/4=20/40`、`2/2=20/20`，允许按项目
规定给少量启动容差。

当前自由运行版本会出现 `target fps did not stabilize`，2 Hz还会出现
`fps-xvs-thin code=-3`，重启流后实测仍约24 fps。因此该项当前结论是**不通过**，
不是“待测”，也不能用电脑端 UVC设置2/4 fps代替。

---

## 9. 阶段 5和6：模拟 Trigger、frame_id、照片和 EXIF

先保持第7节设置后的固定参数。在板卡 Shell预先创建输出父目录：

```bash
mkdir -p /root/camera_uart/selftest_RUN001
```

父目录不存在时，`sync-bind-log` 会返回 `-301`，这不代表绑定算法失败。目录创建好后，
在 `camera-aiq>` 中执行：

```text
time-sync-reset
sync-bind-reset 1
sync-bind-log /root/camera_uart/selftest_RUN001/sync_bind.csv
photo-start 0 /root/camera_uart/selftest_RUN001/photos_cam0
photo-start 1 /root/camera_uart/selftest_RUN001/photos_cam1
sync-sim-start 4 12
wait 5000
sync-sim-status
sync-status
sync-bind-status
sync-bind-last
time-sync-status
photo-stop 0
photo-stop 1
wait 2000
photo-status all
```

通过条件：

- 12个模拟 Trigger全部发出；
- 忽略第一个预备 Trigger后形成11对照片；
- `pending=0`、`trigger_id_gaps=0`、`duplicate_triggers=0`；
- 每路11张 JPEG，`queue_drops/encode_errors/exif_errors/write_errors` 全为0；
- CSV、文件名和 EXIF中的 camera_id、frame_id、trigger_id、曝光、增益、ISO一致。

本项只能写“模拟 Trigger和逐帧绑定软件通过”。`source=SIM`、`utc_valid=0`、
`diagnostic_only=1` 时不能写“硬件同步通过”。

---

## 10. 阶段 7：eMMC短时保存

4000x3000 NV12每帧约18 MB，板卡剩余空间不多时只测试300 ms：

```text
save-start 0 /root/camera_uart/selftest_RUN001/nv12_cam0
save-start 1 /root/camera_uart/selftest_RUN001/nv12_cam1
wait 300
save-stop 0
save-stop 1
wait 3000
capture-status all
```

通过条件分两层判断：两路 `saved>0`、`bytes_saved>0` 且 `save_failures=0`，说明双路
写盘功能可用；只有 `save_queue_drops=0` 才能再判“零丢帧通过”。测试后保留 CSV，
确认无需保留原始帧后再删除大体积 NV12。

本轮短测保存cam0 31帧/558 MB、cam1 30帧/540 MB，`save_failures=0`，但
`save_queue_drops` 分别为13和27。因此双路eMMC写盘功能实机通过，高输入率零丢帧不通过。

---

## 11. 双 UVC、双 HTTP和虚拟网口并发

在 `camera-aiq>` 中：

```text
net-start 0
net-start 1
uvc-start all
wait 5000
net-status all
uvc-status all
capture-status all
```

电脑端先确认节点和 USB速度：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video2 --list-formats-ext
lsusb -t
```

依次测试电脑 UVC输出组合 `4/4`、`4/2`、`2/4`、`2/2`。每次并发运行两条：

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=16 \
  --stream-to=cam0.mjpeg

v4l2-ctl -d /dev/video2 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=2 --stream-mmap=4 --stream-count=8 \
  --stream-to=cam1.mjpeg
```

将 `--set-parm` 和 `--stream-count` 按四种组合替换。用下列命令确认不是空文件：

```bash
ffprobe -v error -count_frames -select_streams v:0 \
  -show_entries stream=width,height,nb_read_frames cam0.mjpeg
ffprobe -v error -count_frames -select_streams v:0 \
  -show_entries stream=width,height,nb_read_frames cam1.mjpeg
```

同时测试网口：

```bash
ping -c 10 192.168.55.1
ssh root@192.168.55.1
curl --max-time 6 -o /dev/null -w '%{http_code} %{size_download}\n' \
  http://192.168.55.1:8080/cam0
curl --max-time 6 -o /dev/null -w '%{http_code} %{size_download}\n' \
  http://192.168.55.1:8080/cam1
```

连续 MJPEG被 `--max-time`中止时 curl返回28是正常的；必须同时看到 HTTP 200和非零
接收字节。当前 `lsusb -t` 为480M，只能判 USB2.0通过，USB3.0不通过。

### 11.1 停止 UVC不能断网

程序中执行：

```text
uvc-stop all
wait 3000
uvc-status all
net-status all
```

电脑端再次执行 ping、SSH和两个 HTTP请求。通过时程序应返回
`usb_gadget=kept rndis=kept`，网络和 HTTP继续工作。随后：

```text
uvc-start all
wait 3000
uvc-status all
```

电脑端再次各取4帧，确认 UVC可以恢复。

本轮 run3 实测：停止期间 UVC 两个节点在7秒窗口内均为0帧，ping 5/5、0%丢包，
重新登录 SSH 成功，双 HTTP 均返回200且收到非零字节；USB保持
`Bus 001 Device 012: ID 2207:0017`。恢复后 cam0取得8帧/4 fps、cam1取得4帧/2 fps，
随后重启 `camera-uvc.service`，服务为active、UDC为configured，最终双 UVC、双 HTTP、
ping和SSH端口冒烟全部通过。这个测试证明软停止 UVC不会主动撤下RNDIS网口。

### 11.2 30分钟并发稳定性测试（无 MCU可执行）

这项测试验证两个相机持续输出时，UVC、HTTP和 RNDIS是否同时保持在线。它不验证
sensor是否真的工作在2/4 Hz，也不验证 XVS硬件同步。

电脑端开五个终端，分别执行：

```bash
timeout 1800s v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --set-parm=4 \
  --stream-mmap=4 --stream-poll \
  --stream-count=0 --stream-to=/dev/null 2>stress30_uvc_cam0.log
timeout 1800s v4l2-ctl -d /dev/video2 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --set-parm=2 \
  --stream-mmap=4 --stream-poll \
  --stream-count=0 --stream-to=/dev/null 2>stress30_uvc_cam1.log
curl --max-time 1800 -o /dev/null -w \
  'http=%{http_code} bytes=%{size_download} speed=%{speed_download}\n' \
  http://192.168.55.1:8080/cam0 2>stress30_http_cam0.log
curl --max-time 1800 -o /dev/null -w \
  'http=%{http_code} bytes=%{size_download} speed=%{speed_download}\n' \
  http://192.168.55.1:8080/cam1 2>stress30_http_cam1.log
ping 192.168.55.1 | tee stress30_ping.log
```

第六个终端每分钟在板端记录一次服务状态、UDC状态、内存、温度和
`camera_aiq_test`进程。30分钟后必须看到：HTTP为200且字节数非零，ping为0%丢包，
两路UVC日志持续出现 fps。若日志有 `dropped buffers`，必须记录，不能填写“零丢帧”。

本轮 run3 实测：cam0有1799个速率样本，平均4.134 fps（4.03～5.90 fps），202行
dropped-buffer提示、累计460个buffer；cam1有1785个速率样本，平均2.120 fps
（2.03～3.07 fps），180行提示、累计408个buffer。HTTP分别收到
16,088,441,756字节和14,902,341,180字节，均为HTTP 200；ping为1783/1783、0%丢包。
31个板端监控样本全部为服务active、UDC configured，USB链路均为high-speed。
因此判定为“30分钟持续有流并且网络稳定，但非零丢帧”；不能填写“零丢帧通过”。

---

## 12. 测试完成后恢复常驻服务

在交互程序输入：

```text
quit
```

回到板卡 Shell：

```bash
systemctl restart camera-uvc.service
sleep 8
systemctl status camera-uvc.service --no-pager
cat /sys/class/udc/*/state
ip -br addr show usb0
pgrep -a camera_aiq_test
```

最终应看到：服务 `active (running)`、UDC `configured`、`usb0` 为 UP且地址为
`192.168.55.1/24`、只有一个 `camera_aiq_test --all-daemon`。电脑端再做一次双 UVC、
双 HTTP、ping和 SSH冒烟测试。

---

## 13. 当前应怎样填写阶段结论

| 阶段 | 无 MCU当前结论 |
|---:|---|
| 1 | 实机通过：双 IMX586同时真实出图 |
| 2 | 实机通过：曝光、增益、ISO映射独立；两份 IQ内容仍相同 |
| 3 | 不通过：sensor底层2/4 Hz未形成；30分钟并发长稳只能证明输出链路持续工作；功耗待仪器 |
| 4 | 软件通过：115200、8N1及协议自检通过；真实 MCU UART待验收 |
| 5 | 软件通过：模拟 Trigger/frame_id绑定通过；真实 XVS待验收 |
| 6 | 软件通过：JPEG/EXIF/CSV/参数绑定通过；真实 UTC和曝光相位待硬件验收 |
| 7 | USB2.0功能实机通过：双 UVC、双 HTTP、RNDIS/SSH和短时 eMMC可工作；UVC/eMMC零丢帧及USB3.0不通过 |

已知稳定性风险必须记录：USB2.0下出现过一次损坏 UVC缓冲、非单调时间戳提示，主机
关闭 UVC流时 DWC3有 endpoint request未入队及 `No such device (19)`告警。30分钟
连续输出没有掉线，但缓冲丢弃和主机重协商稳定性问题仍未关闭。

---

## 14. 接入 MCU后从哪里继续

1. 先用示波器确认 `FSYNC_CAM` 空闲高、4 Hz、周期约250 ms、低脉冲约10 us。
2. 确认 UART9电平为1.8 V；3.3 V MCU必须经过电平转换，TX/RX交叉并共地。
3. MCU的 UART `PING` 和 `IDLE` 必须返回合法 ACK/PONG。
4. MCU先持续输出4 Hz XVS，再刷入已经备份的 XVS从模式镜像。
5. 重启后先确认两路各约4 fps，再测40/40、40/20、20/40、20/20帧增量。
6. 示波器同步观察 XVS和两路帧/曝光信号，记录相位差与 Trigger到帧事件延迟。
7. 接 PPS/GPRMC后要求 `time-sync-status` 进入 LOCKED且 `utc_valid=1`。
8. 最后重跑照片 EXIF、双 UVC、双 HTTP、eMMC、30～60分钟长稳和功耗测试。
