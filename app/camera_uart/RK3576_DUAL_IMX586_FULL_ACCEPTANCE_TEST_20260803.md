# RK3576 双 IMX586 全功能测试与验收手册

版本：V1.0  
测试日期：2026-08-03  
适用平台：RK3576 + 两路 IMX586 + Linux 6.1  
适用程序：`/root/camera_uart/camera_aiq_test`  
文档目标：没有接触过本项目的人，也能区分电脑端和板卡端，按顺序完成测试并得出正确结论。

---

## 1. 先看结论

本次不是“所有需求全部验收通过”。相机软件和主要输出链路已经可用，但仍有必须解决或补测的项目。

### 1.1 本次通过的项目

| 项目 | 实测结论 | 主要证据 |
| --- | --- | --- |
| 双 IMX586 同时工作 | 通过 | cam0、cam1 同时采集、同时 UVC 出图 |
| 两路独立 IQ | 通过 | 分别加载 `/etc/iqfiles/cam0`、`/etc/iqfiles/cam1` |
| 双路 UVC | 通过 | 电脑枚举 `/dev/video0` 和 `/dev/video2` 两路图像节点 |
| UVC 分辨率 | 通过 | 两路抓图均为 4000x3000 JPEG |
| UVC 长度测试 | 基本通过 | 两路同时各取 1000 帧，均正常结束，约 9.966 fps |
| UVC 按路启停 | 通过 | 停 cam0 时 cam1 连续；停 cam1 时 cam0 连续 |
| 独立曝光和增益 | 通过 | 单独修改一路时，目标画面明显变化，另一路亮度基本不变 |
| 双路 eMMC 保存 | 短时通过 | 两路各保存 10 个 18,000,000 字节 NV12，无保存队列丢弃和写错误 |
| HTTP MJPEG 后端 | 板内通过 | cam0、cam1 均返回 HTTP 200，5 秒持续收到图像数据 |
| UART 控制协议软件 | 通过 | 12 个协议用例、115200/8N1 伪终端收发和 ACK 均通过 |
| 模拟触发、照片和 EXIF 软件链 | 通过 | 两路各保存 5 张绑定 trigger_id 的 JPEG，无编码、EXIF、写文件错误 |
| 开机 UVC 服务 | 通过 | `camera-uvc.service` 已启用、运行中、重启次数为 0 |

### 1.2 本次没有通过的项目

| 项目 | 结论 | 原因或实测值 |
| --- | --- | --- |
| 两路源采集 2 Hz/4 Hz | **失败** | 下发 cam0=4、cam1=2 后，两路实测仍约 26.3 fps；RKAIQ 状态仍为 10 fps，驱动模式为 30 fps |
| ISO 有效回读 | **失败** | `status` 中 ISO 始终为 0；增益控制有效，但 0 不能作为真实 ISO 交付 |
| 严格全程零丢帧 | **未达到** | 两路 UVC 启动时各报告 2 个 dropped buffers；稳定段能持续运行 |
| 正确 UTC/绝对 EXIF 时间 | **失败** | 板卡系统时间显示为 2026-06-06，实际测试日期为 2026-08-03 |

### 1.3 因缺少硬件或测试时长，本次不能下结论的项目

| 项目 | 当前状态 | 还需要什么 |
| --- | --- | --- |
| 真实 UART 线双向命令/ACK | 未实线复测 | 1.8 V UART 对端或合适的电平转换器 |
| 两颗 IMX586 真实 XVS 同步 | 未验证 | MCU、FSYNC_CAM、两路 XVS、示波器/逻辑分析仪 |
| PPS + GPRMC/NMEA + UTC | 未验证 | GPS/授时模块、PPS 和 NMEA 串口数据 |
| 1000 次物理触发零丢帧 | 未验证 | 真实 2 Hz/4 Hz XVS 模式和约 250/500 秒测试 |
| 电脑直接访问板卡 HTTP | 当前连接条件不具备 | 板卡以太网、USB RNDIS/ECM，或 UVC+网卡复合 gadget |
| 30～60 分钟长稳 | 未执行 | 持续运行时间、日志和监控记录 |
| 整机功耗、DDR/ISP/NPU 占用 | 未完整测试 | 外置功率计和平台性能监控工具 |
| RAW10 极限和 YOLO | 不在本次程序回归范围 | RAW 节点测试和 YOLO 程序/模型 |

> 验收原则：软件模拟同步通过，不等于两颗传感器已经硬件同步；板内 HTTP 通过，也不等于电脑和板卡之间已经有网络。

---

## 2. 认识设备和三种“串口/USB”

### 2.1 本项目固定映射

| 名称 | camera_id | 传感器 | IQ 文件 | 板端采集节点 | 板端 UVC 输出节点 | 本次电脑 UVC 图像节点 |
| --- | ---: | --- | --- | --- | --- | --- |
| camera 0 | 0 | `m00_b_imx586 4-001a` | `/etc/iqfiles/cam0/imx586_default_default.json` | `/dev/video22` | `/dev/video49` | `/dev/video0` |
| camera 1 | 1 | `m01_b_imx586 5-001a` | `/etc/iqfiles/cam1/imx586_default_default.json` | `/dev/video31` | `/dev/video50` | `/dev/video2` |

电脑上的 `/dev/video1` 和 `/dev/video3` 是 metadata 节点，不能当作图像节点抓图。电脑重启或重新插拔后，节点编号可能变化，所以每次必须重新枚举，不能永远假定是 0 和 2。

### 2.2 不要混淆两个串口

| 用途 | 设备 | 波特率 | 说明 |
| --- | --- | ---: | --- |
| 板卡调试登录 | 电脑上的 `/dev/ttyUSB0`（编号可能变化） | 1500000 | 进入板卡 Linux 命令行 |
| 相机控制 UART | 板卡上的 `/dev/ttyS9` | 115200、8N1 | MCU/上位机向 `camera_aiq_test` 发相机命令 |

调试串口验证通，不代表 `/dev/ttyS9` 的外部实线控制已经验证。两者是不同链路。

### 2.3 Type-C 的作用

RK3576 的 Type-C OTG 口在本测试中工作为 USB Device，电脑将它识别成两个 UVC 摄像头。它不需要再接一个摄像头；IMX586 已经接在板卡的 MIPI 接口上，Type-C 负责把处理后的画面送给电脑。

---

## 3. 测试前准备

### 3.1 需要的硬件

- RK3576 板卡和独立电源。
- 两颗已经连接的 IMX586。
- 一根确认能传数据的 Type-C 转 USB-A 数据线。
- 调试串口线。
- 测真实控制 UART 时，增加 1.8 V UART 对端或电平转换器。
- 测真实同步时，增加 MCU、PPS/GPS 模块和示波器/逻辑分析仪。

控制 UART 所在 VCCIO 为 1.8 V。不要把普通 3.3 V 或 RS-232 电平直接接入 `/dev/ttyS9` 对应引脚。

### 3.2 电脑安装工具

以下命令在**电脑端终端**执行：

```bash
sudo apt update
sudo apt install -y v4l-utils ffmpeg usbutils picocom curl exiftool
```

检查工具：

```bash
v4l2-ctl --version
ffmpeg -version
lsusb
```

### 3.3 登录板卡

以下命令在**电脑端终端**执行：

```bash
picocom -b 1500000 /dev/ttyUSB0
```

看到登录提示后，以 `root` 登录。退出 picocom 使用 `Ctrl+A`，再按 `Ctrl+X`。

如果 `/dev/ttyUSB0` 不存在，先执行：

```bash
ls -l /dev/ttyUSB*
```

选择实际的调试串口节点。

### 3.4 测试期间的基本规则

1. 同一时刻只允许一个相机程序占用 `/dev/video22`、`/dev/video31` 和 RKAIQ。
2. `camera-uvc.service` 运行时，不要再启动第二个 `camera_aiq_test`。
3. 不要在板端另开 `v4l2-ctl` 直接占用 `/dev/video22` 或 `/dev/video31`。
4. 交互测试前先停止服务，结束后一定恢复服务。
5. 修改曝光和增益之间必须等待至少 1 秒，原因见第 8.4 节。

---

## 4. 基线检查：先确认程序、IQ 和服务

本节命令都在**板卡端**执行。

### 4.1 检查程序版本

```bash
sha256sum /root/camera_uart/camera_aiq_test
```

本次验收版本应为：

```text
4f344e04072799327057f0277ef37d56dfe99dca71aedea6c0f52792bda115b6
```

哈希不一致时，说明不是本次测试的同一程序，不能直接套用本报告结论。

### 4.2 检查两份独立 IQ

```bash
ls -lh /etc/iqfiles/cam0/imx586_default_default.json
ls -lh /etc/iqfiles/cam1/imx586_default_default.json
```

通过条件：两个文件都存在。仅仅把文件放在目录里还不够，启动日志还必须显示 cam0 使用 cam0 文件、cam1 使用 cam1 文件。

### 4.3 检查开机服务

```bash
systemctl is-enabled camera-uvc.service
systemctl is-active camera-uvc.service
systemctl status camera-uvc.service --no-pager
journalctl -u camera-uvc.service -n 120 --no-pager
```

通过条件：

- `is-enabled` 输出 `enabled`；
- `is-active` 输出 `active`；
- 日志包含 `CAMERA_BACKEND_READY cameras=2`；
- 日志包含 `UVC_AUTOSTART_READY cameras=0,1 outputs=2 mode=4000x3000@10fps/MJPEG`；
- 日志包含 `CONTROL_UART_READY device="/dev/ttyS9" baud=115200 format=8N1`；
- 日志中 cam0、cam1 分别显示各自的 IQ 路径。

### 4.4 检查系统时间

```bash
date -Ins
```

通过条件：时间和当前标准时间一致。时间错误时，普通抓图仍能工作，但文件时间和 EXIF 的绝对 UTC 时间不能通过验收。

---

## 5. 电脑端双 UVC 枚举和预览

本节默认 `camera-uvc.service` 正在运行，Type-C 数据线已经连接电脑。

### 5.1 确认 USB 枚举

在**电脑端**执行：

```bash
lsusb
v4l2-ctl --list-devices
```

本次实测看到：

```text
2207:0005 Fuzhou Rockchip Electronics Company rk3xxx
```

并枚举出 `/dev/video0`、`/dev/video1`、`/dev/video2`、`/dev/video3`。

### 5.2 找出真正的两个图像节点

在**电脑端**逐个执行：

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video1 --list-formats-ext
v4l2-ctl -d /dev/video2 --list-formats-ext
v4l2-ctl -d /dev/video3 --list-formats-ext
```

支持 `MJPG 4000x3000` 的两个节点才是图像节点。本次是 `/dev/video0` 和 `/dev/video2`。

### 5.3 分别抓一张图

在**电脑端**执行：

```bash
mkdir -p ~/rk3576_acceptance

v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=10 --stream-mmap=4 --stream-count=1 \
  --stream-to=~/rk3576_acceptance/cam0.jpg

v4l2-ctl -d /dev/video2 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=10 --stream-mmap=4 --stream-count=1 \
  --stream-to=~/rk3576_acceptance/cam1.jpg

file ~/rk3576_acceptance/cam0.jpg
file ~/rk3576_acceptance/cam1.jpg
```

通过条件：两个文件都是 `JPEG image data`，分辨率均为 4000x3000，而且画面分别来自两颗摄像头。

### 5.4 实时预览

在电脑端开两个终端：

```bash
ffplay -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 10 /dev/video0
```

```bash
ffplay -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 10 /dev/video2
```

关闭窗口即可停止预览。主机关闭 UVC 流时，板端可能短暂记录 `No such device (19)`，这是主机释放流的现象；重新打开能恢复时不判定为相机故障。

---

## 6. 双 UVC 同时 1000 帧稳定性测试

在**电脑端同一个终端**执行：

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=10 --stream-mmap=4 --stream-count=1000 \
  --stream-to=/dev/null &
PID0=$!

v4l2-ctl -d /dev/video2 \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=10 --stream-mmap=4 --stream-count=1000 \
  --stream-to=/dev/null &
PID1=$!

wait "$PID0"
RC0=$?
wait "$PID1"
RC1=$?
echo "cam0_rc=$RC0 cam1_rc=$RC1"
```

测试约需要 100 秒。通过条件：

- 两个返回码都是 0；
- 两路都走完 1000 帧，不能中途不再增长；
- 平均帧率接近 10 fps；
- `journalctl -u camera-uvc.service` 中无 MPP 编码错误、相机异常退出或服务重启。

严格验收还要求 `dropped buffers=0`。本次实测两路返回码均为 0，耗时约 100.34 秒，平均约 9.966 fps，但两路在启动阶段各报告 2 个 dropped buffers，所以“连续工作”通过，“严格零丢帧”未通过。

---

## 7. 进入交互测试模式

曝光、增益、帧率、保存、模拟同步等命令需要在程序提示符中测试。本节命令在**板卡端**执行。

### 7.1 停止常驻服务

```bash
systemctl stop camera-uvc.service
systemctl stop camera-http.service 2>/dev/null || true
pgrep -a camera_aiq_test
pgrep -a rkaiq_3A_server
```

通过条件：两个 `pgrep` 都没有输出。如果还有旧进程，先确认它的来源，不要直接再启动第二个程序。

### 7.2 启动同一个正式程序

```bash
cd /root/camera_uart
./camera_aiq_test
```

看到 `camera-aiq>` 提示符后输入：

```text
stream-start all
wait 3000
status all
capture-status all
uvc-start all
wait 3000
uvc-status all
```

为什么必须先 `stream-start all`：RKAIQ 的 `prepare` 只完成参数准备，真正开始取帧后，曝光结果、帧号、保存、UVC 和网络后端才有数据。没有采集帧时执行曝光/状态命令会出现 `camera is not ready`，这不是 UART 本身的问题。

通过条件：两路 `running=1`，`frames` 持续增加，两路 UVC 都是 `enabled=1`。

---

## 8. 曝光和增益独立性测试

本测试需要同时使用：

- **板卡端**：在 `camera-aiq>` 输入控制命令；
- **电脑端**：从两个 UVC 图像节点抓图。

### 8.1 camera 0 低曝光/低增益

板卡端输入：

```text
exposure 0 5000
wait 1000
gain 0 1000
wait 2000
status all
```

电脑端输入：

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --stream-mmap=4 --stream-count=1 --stream-to=~/rk3576_acceptance/cam0_low.jpg
v4l2-ctl -d /dev/video2 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --stream-mmap=4 --stream-count=1 --stream-to=~/rk3576_acceptance/cam1_when_cam0_low.jpg
```

### 8.2 camera 0 高曝光/高增益

板卡端输入：

```text
exposure 0 30000
wait 1000
gain 0 8000
wait 2000
status all
```

电脑端输入：

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --stream-mmap=4 --stream-count=1 --stream-to=~/rk3576_acceptance/cam0_high.jpg
v4l2-ctl -d /dev/video2 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --stream-mmap=4 --stream-count=1 --stream-to=~/rk3576_acceptance/cam1_when_cam0_high.jpg
```

通过条件：cam0 明显变亮；cam1 没有跟着发生同方向的大幅变化。

### 8.3 camera 1 重复同样测试

先让 cam0 保持不变，板卡端输入：

```text
exposure 1 5000
wait 1000
gain 1 1000
wait 2000
status all
```

电脑端分别保存 cam0、cam1 图像。然后板卡端输入：

```text
exposure 1 30000
wait 1000
gain 1 8000
wait 2000
status all
```

电脑端再次分别保存两路图像。通过条件：cam1 明显变亮，cam0 基本不变。

### 8.4 为什么曝光和增益之间要等待

当前 `exposure` 命令会保留“它查询到的当前增益”，`gain` 命令会保留“它查询到的当前曝光”。RKAIQ 参数应用到传感器有帧延迟。如果两条命令紧挨着发送，第二条命令可能读到旧曝光，并把刚设置的曝光覆盖回去。

所以当前版本必须这样操作：

```text
exposure CAMERA_ID EXPOSURE_US
wait 1000
gain CAMERA_ID GAIN_X1000
wait 1000
```

这属于已知的命令时序限制。后续程序应在后端缓存目标曝光/增益，或提供原子 `manual` 命令，才能去掉人工等待。

### 8.5 恢复自动曝光

板卡端输入：

```text
auto 0
auto 1
wait 3000
status all
```

### 8.6 本次实测亮度数据

使用 FFmpeg `signalstats` 得到的平均亮度 YAVG：

| 测试图 | YAVG |
| --- | ---: |
| cam0，5000 us、1x | 8.427 |
| cam0，30000 us、8x | 59.120 |
| cam1 在 cam0 低/高设置期间 | 85.118 / 87.573 |
| cam1，5000 us、1x | 9.339 |
| cam1，30000 us、8x | 60.789 |
| cam0 在 cam1 低/高设置期间 | 59.047 / 57.863 |

该结果证明目标相机亮度发生明显变化，同时另一路没有被同一命令串改。

---

## 9. 帧率控制测试

在**板卡交互提示符**输入：

```text
fps 0 4
fps 1 2
wait 3000
status all
capture-status all
```

记下两路第一次的 `frames`，等待 20 秒：

```text
wait 20000
capture-status all
```

计算：

```text
实际 FPS = （第二次 frames - 第一次 frames）/ 20
```

通过条件：cam0 接近 4 fps，cam1 接近 2 fps，允许误差应在项目正式指标中固定，例如不超过 1%。

本次实测：

```text
cam0: 7183 - 6657 = 526 帧，526 / 20 = 26.3 fps
cam1: 6255 - 5729 = 526 帧，526 / 20 = 26.3 fps
```

因此当前版本的 2/4 fps 控制判定失败。当前 IQ 中 `CISMinFps` 为 10，RKAIQ 状态仍报告 10 fps，IMX586 驱动模式日志为 4000x3000@30。需要继续修改 sensor VBLANK/帧长控制和 IQ 最小帧率边界，再做真实帧计数验收。不能因为命令返回 `OK` 就判定帧率成功。

测试后可输入：

```text
fps 0 10
fps 1 10
```

---

## 10. ISO 和增益测试的正确判断

`gain CAMERA_ID GAIN_X1000` 设置的是模拟增益，`8000` 表示约 8 倍，不是 ISO 8000。

板卡端输入：

```text
gain 0 1000
wait 1000
status 0
gain 0 8000
wait 1000
status 0
```

通过条件应包括：

- 增益实际回读从约 1000 变到约 8000；
- 图像亮度/噪声发生相应变化；
- ISO 返回合理的非零值，并说明 ISO 的计算或标定关系。

本次增益和画面变化通过，但 ISO 一直是 0，所以“增益控制”通过，“ISO 回读”失败。当前不能把 0 写成有效 `PhotographicSensitivity` 交付。

---

## 11. 双路 eMMC 保存测试

### 11.1 短时功能测试

有 SSH 或第二个调试终端时，先在板卡另一个 shell 检查空间：

```bash
df -h /root
```

如果现场只有一个调试串口，先在交互程序中完成保存，等第 19 节退出程序后再检查文件；不要为了查看目录而直接断掉正在运行的程序。

4000x3000 NV12 每帧大小为：

```text
4000 x 3000 x 3 / 2 = 18,000,000 字节
```

在 `camera-aiq>` 输入：

```text
save-start 0 /root/camera_acceptance/emmc_cam0
save-start 1 /root/camera_acceptance/emmc_cam1
wait 1000
save-stop 0
save-stop 1
capture-status all
```

在板卡 shell 检查：

```bash
find /root/camera_acceptance/emmc_cam0 -type f -name '*.nv12' -printf '%f %s\n'
find /root/camera_acceptance/emmc_cam1 -type f -name '*.nv12' -printf '%f %s\n'
wc -l /root/camera_acceptance/emmc_cam0/cam0_frames.csv
wc -l /root/camera_acceptance/emmc_cam1/cam1_frames.csv
```

通过条件：

- 每个 NV12 都是 18,000,000 字节；
- CSV 数据行和完整图片文件一一对应；
- `saving=0`、`save_queue_pending=0`；
- `save_queue_drops=0`、`save_failures=0`、`last_errno=0`。

本次两路同时各保存 10 帧、各 180,000,000 字节，以上错误计数均为 0，短时功能通过。

### 11.2 30～60 分钟长稳测试

正式验收还需在目标 2 Hz/4 Hz 下同时保存 30～60 分钟。每 5 分钟记录一次：

```text
capture-status all
status all
```

结束后检查：文件数、CSV 行数、磁盘空间、保存丢弃、序号丢帧、内存增长和温度。当前 2/4 fps 尚未真正生效，所以长稳测试必须在帧率修复后重做。

---

## 12. UVC 按相机独立启停

板卡提示符输入：

```text
uvc-status all
uvc-stop 0
wait 3000
uvc-status all
```

此时电脑端让 cam1 连续取 150 帧：

```bash
v4l2-ctl -d /dev/video2 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --stream-mmap=4 --stream-count=150 --stream-to=/dev/null
```

板卡端恢复 cam0：

```text
uvc-start 0
wait 3000
uvc-stop 1
```

电脑端让 cam0 连续取 150 帧：

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4000,height=3000,pixelformat=MJPG --stream-mmap=4 --stream-count=150 --stream-to=/dev/null
```

最后板卡端输入：

```text
uvc-start 1
uvc-status all
```

通过条件：停止一路时另一路 150 帧完整结束，USB 设备不整体重新枚举，恢复后被停止的一路能重新打开。本次双向隔离测试均通过。

---

## 13. HTTP 网口输出测试

### 13.1 URL 的含义

```text
http://板卡IP:8080/cam0
http://板卡IP:8080/cam1
http://板卡IP:8080/
```

这里的“板卡IP”必须是 RK3576 在电脑可达网络上的地址。电脑浏览器里的 `127.0.0.1` 是电脑自己，不是板卡。只有做了 SSH 端口转发时，电脑才使用转发后的 `127.0.0.1`。

客户正常使用不应该每次手动执行 SSH 隧道。正式产品应提供实际以太网，或把 USB gadget 配成 UVC + RNDIS/ECM 复合设备，并固定板卡 IP。

### 13.2 交互模式启动两路 HTTP

板卡提示符输入：

```text
net-start 0
net-start 1
wait 3000
net-status all
```

如果板卡有网络地址，在板卡 shell 查询：

```bash
ip -br addr
```

假设板卡地址是 `10.200.2.67`，电脑浏览器打开：

```text
http://10.200.2.67:8080/
```

不要把电脑自己的 IP 填成板卡 IP。

### 13.3 板内回环测试

即使当前没有板卡到电脑的网络，也可以在**板卡第二个 shell**验证 HTTP 服务本身：

```bash
curl --max-time 5 -o /dev/null \
  -w 'cam0 http=%{http_code} bytes=%{size_download}\n' \
  http://127.0.0.1:8080/cam0

curl --max-time 5 -o /dev/null \
  -w 'cam1 http=%{http_code} bytes=%{size_download}\n' \
  http://127.0.0.1:8080/cam1
```

MJPEG 是无限流，5 秒后 curl 返回超时码 28 是预期行为；要看的是 HTTP 状态为 200，而且 `bytes` 持续大于 0。

如果现场只有一个调试串口、没有第二个 shell，就先在 `camera-aiq>` 中输入：

```text
net-stop all
uvc-stop all
stream-stop all
quit
```

回到板卡 shell 后，改用 HTTP 常驻服务做回环测试：

```bash
systemctl stop camera-uvc.service
systemctl restart camera-http.service
sleep 5
systemctl is-active camera-http.service

curl --max-time 5 -o /dev/null \
  -w 'cam0 http=%{http_code} bytes=%{size_download}\n' \
  http://127.0.0.1:8080/cam0

curl --max-time 5 -o /dev/null \
  -w 'cam1 http=%{http_code} bytes=%{size_download}\n' \
  http://127.0.0.1:8080/cam1

systemctl stop camera-http.service
```

本次结果：cam0 5 秒收到 35,615,810 字节，cam1 收到 31,256,999 字节，均为 HTTP 200。当前 USB 配置是纯 UVC，板卡只有 `lo` 和 `dummy0`，没有电脑可达的网络接口，所以本次只能证明 HTTP 后端工作，不能证明电脑直连访问。

### 13.4 HTTP 状态判断

`net-status all` 中：

- `encode_errors=0` 和 `http_errors=0` 是必须条件；
- 客户端主动关闭后出现一次 socket `EPIPE`/errno 32 可以作为断开事件记录；
- 当前采集源约 26 fps，而网络只编码最新的 10 fps，`queue_drops` 增长主要表示主动丢弃旧帧，不等同于 MIPI 丢帧；
- 但正式目标是源端 2～4 fps，修复源帧率后仍要复测。

---

## 14. UART 115200、8N1 控制测试

### 14.1 软件自测

如果还停留在 `camera-aiq>`，先输入下面几条，正常退出交互程序：

```text
net-stop all
uvc-stop all
stream-stop all
quit
```

然后在**板卡端 shell**停止所有相机服务并执行软件自测：

```bash
systemctl stop camera-uvc.service
systemctl stop camera-http.service 2>/dev/null || true
cd /root/camera_uart
./camera_aiq_test --control-uart-protocol-self-test
```

通过标志：

```text
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14 pty=115200_8N1_ACK"
```

开发电脑源码目录还可以执行：

```bash
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart
make -f Makefile.camera_aiq check-control-uart-host
```

本次软件测试 14 个协议用例均通过，包括 115200、8N1 伪终端收发和 ACK。它不证明实际 TX/RX 引脚、电平转换或 MCU 程序正确。

### 14.2 实际接线

| RK3576 | 对端 | 说明 |
| --- | --- | --- |
| CN4 pin 19 `UART_CAM1_TX` | 对端 RX | RK3576 返回 ACK/状态 |
| CN4 pin 17 `UART_CAM1_RX` | 对端 TX | 对端发送控制命令 |
| GND | 对端 GND | 必须共地 |

参数：115200 baud、8 data bits、no parity、1 stop bit、无流控。VCCIO 为 1.8 V，3.3 V 对端必须加正确的电平转换。

### 14.3 启动正式 UART 服务

板卡端执行：

```bash
systemctl start camera-uvc.service
journalctl -u camera-uvc.service -n 80 --no-pager
```

必须看到：

```text
CONTROL_UART_READY device="/dev/ttyS9" baud=115200 format=8N1 protocol=CAM_V1
```

### 14.4 CAM_V1 常用请求

每条请求以 `\r\n` 结束：

```text
$CAM,1,255,PING
$CAM,2,255,GET_STATUS
$CAM,3,255,STREAM_START
$CAM,4,0,UVC_STOP
$CAM,5,0,UVC_START
$CAM,6,1,UVC_STATUS
$CAM,7,0,NET_START
$CAM,8,1,NET_STOP
$CAM,9,0,SAVE_START,/root/uart_save/cam0
$CAM,10,0,SAVE_STOP
$CAM,11,0,EXPOSURE,30000
$CAM,12,1,GAIN,8000
$CAM,13,1,FPS,10
```

`camera_id` 为 0 或 1；全局/全部使用 255。每条命令都必须收到相同 sequence 和 camera_id 的 `$ACK`。无回复、`$NACK`、序号不一致或控制了错误相机都判失败。

### 14.5 必须做的实线矩阵

| 命令 | camera_id=0 | camera_id=1 | 通过条件 |
| --- | --- | --- | --- |
| PING/GET_STATUS | 测 | 测 | ACK 和状态可解析 |
| EXPOSURE | 测 | 测 | 只改变指定相机 |
| GAIN | 测 | 测 | 只改变指定相机 |
| FPS | 测 | 测 | 实际帧计数达到目标，不能只看 ACK |
| UVC_STOP/START | 测 | 测 | 另一路不中断 |
| NET_STOP/START | 测 | 测 | URL 和状态按路变化 |
| SAVE_START/STOP | 测 | 测 | 文件和 CSV 按路产生 |
| GET_STATUS | 测 | 测 | 返回值与实际状态一致 |

本次没有外部真实控制 UART 对端，因此此矩阵还没有实线验收，不能只用软件自测替代。

---

## 15. 软件模拟同步、照片和 EXIF 测试

以下测试只验证软件数据链，不驱动 IMX586 的物理 XVS。

正式 UART 服务会占用相机。先在板卡 shell 切回交互模式：

```bash
systemctl stop camera-uvc.service
systemctl stop camera-http.service 2>/dev/null || true
cd /root/camera_uart
./camera_aiq_test
```

看到板卡交互提示符后输入：

```text
stream-start all
photo-offset 0 0
photo-offset 1 0
photo-start 0 /root/camera_acceptance/photo_cam0
photo-start 1 /root/camera_acceptance/photo_cam1
sync-bind-reset 1
sync-sim-start 2 6
wait 5000
sync-sim-stop
photo-stop 0
photo-stop 1
photo-status all
sync-bind-last
```

本次每路保存 5 张 JPEG：第一次触发作为 pre-shutter 被忽略，后续 5 次绑定到两路 frame sequence。编码、EXIF、写文件错误均为 0。

用板卡或电脑上的 `exiftool` 检查：

```bash
exiftool -DateTimeOriginal -SubSecTimeOriginal -ExposureTime \
  -PhotographicSensitivity -UserComment \
  /root/camera_acceptance/photo_cam0/*.jpg
```

注意：

- 软件模拟触发的 `utc_valid=0`；
- 当前 ISO 为 0 时，EXIF ISO 不是合格的真实测量值；
- 当前曝光状态来自 RKAIQ 最新值，不是严格的逐帧寄存器记录；
- `photo-offset=0` 只适合软件测试，真实响应延迟必须用示波器标定。

---

## 16. 真实硬件同步验收

### 16.1 当前为什么不能判定同步完成

本次 `sync-bind-last` 的两路 frame 时间差约 42.636 ms，这只是自由运行相机中应用层选择到的两帧，不是同一 XVS 沿触发的曝光。当前运行 DTS 未发现 `sony,xvs-slave-mode`、FSIN/Trigger/PPS GPIO 配置，驱动日志仍显示 4000x3000@30 自由运行。

因此当前只能说“trigger_id/frame_id 软件绑定链路工作”，不能说“两颗 IMX586 已硬件同步”。

### 16.2 XVS 接线

| RK3576/CN4 | MCU | 说明 |
| --- | --- | --- |
| pin 19 `UART_CAM1_TX` | MCU UART RX | RK3576 发 XVS 命令 |
| pin 17 `UART_CAM1_RX` | MCU UART TX | MCU 返回 ACK/状态 |
| pin 23 `FSYNC_CAM` | MCU 定时器输出 | 空闲高、低脉冲有效 |
| GND | MCU GND | 必须共地 |

XVS 应使用硬件定时器/PWM，不能依赖 MCU 主循环延时翻转 GPIO。4 Hz 周期 250,000 us，2 Hz 周期 500,000 us，低脉冲建议 10 us，空闲保持高电平。

### 16.3 1000 次物理触发命令

只有真实 MCU、XVS 接线和正确电平都准备好后才执行本节。先退出第 15 节的普通交互模式：

```text
stream-stop all
quit
```

再在板卡 shell 确认服务已停止，并以 XVS 控制模式启动 `camera_aiq_test`：

```bash
systemctl stop camera-uvc.service
systemctl stop camera-http.service 2>/dev/null || true
cd /root/camera_uart
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

在提示符输入 4 Hz 测试：

```text
sync-idle
stream-start all
sync-count 4 1
wait 1000
capture-status all
sync-count 4 1000
wait 251000
sync-controller-status
capture-status all
sync-status
sync-stop
stream-stop all
```

2 Hz 时 1000 个脉冲约 500 秒。通过条件：

- MCU 实际脉冲累计准确增加 1000；
- 两路帧数各增加 1000；
- 两路帧数差为 0；
- `sequence_drops=0`；
- 停止后两路帧数不再增加；
- 示波器同时测 `FSYNC_CAM`、`PPS_OUT`、cam0 XVS 和 cam1 XVS，得到最大值和 P99 同步偏差；
- trigger_id 和两路 frame_id 一一对应，无重复、无漏绑。

### 16.4 单 UART 架构

最终只有一个 MCU、一个 `/dev/ttyS9`。使用 `--uart /dev/ttyS9` 后，程序只打开
一次串口并统一处理相机命令、XVS 控制应答、PPS/NMEA/Trigger 事件。主机伪终端
已经验证三类消息交错和控制命令嵌套查询 XVS 不死锁；真实引脚、电平和 MCU
固件仍必须按本章完成上板验收。

---

## 17. 资源、温度、功耗和长稳测试

### 17.1 本次短时实测

UVC 服务运行约 18 秒后：

| 指标 | 实测值 |
| --- | ---: |
| `camera_aiq_test` CPU | 约 14.6% |
| RSS 内存 | 约 276,716 KiB |
| 内存占用比例 | 约 6.9% |
| 最高温度 | 约 50.846 摄氏度 |
| 服务重启次数 | 0 |
| `/root` 剩余空间 | 约 11 GiB |

这些是短时样本，不是功耗和长稳结论。

### 17.2 正式 30～60 分钟测试方法

在板卡端每分钟记录一次：

```bash
date -Ins
ps -o pid,%cpu,%mem,rss,vsz,etime,cmd -C camera_aiq_test
free -m
df -h /root
for z in /sys/class/thermal/thermal_zone*/temp; do echo "$z $(cat "$z")"; done
systemctl show camera-uvc.service -p NRestarts -p ActiveState -p SubState
```

电脑端让两路 UVC 持续取帧，或按需要保存到高速磁盘。正式记录至少包括：

- 总帧数、平均 FPS、最小/最大帧间隔；
- UVC、V4L2、保存队列和 MIPI/CSI 错误；
- 进程 RSS 起始值和结束值；
- 各温度点最大值；
- 待机、单路、双路、UVC、HTTP、eMMC、同步、YOLO 各工况的外置功率计读数。

没有外置功率计时，不能把软件显示的 CPU 占用当作整机功耗。

---

## 18. 常见故障排查

### 18.1 电脑完全看不到 `2207:0005`

依次检查：

1. Type-C 口是否确实是 OTG/Device 口；
2. 数据线是否支持数据，不只是充电；
3. 板卡是否独立供电并完全启动；
4. `camera-uvc.service` 是否 active；
5. 板端 `/sys/class/udc/*/state` 是否为 `configured`；
6. 电脑 `dmesg -w` 在插拔时是否有新 USB 设备；
7. gadget 是否包含 `uvc.0` 和 `uvc.1`。

`UDC state = not attached` 表示电脑和设备控制器没有完成物理连接/枚举，程序即使启动也没有主机可发送。

### 18.2 枚举四个 `/dev/video*`，但只有两个能出图

这是正常的 UVC image + metadata 枚举。用 `--list-formats-ext` 选择支持 MJPG 4000x3000 的两个图像节点。

### 18.3 `camera is not ready`

通常原因：只初始化 RKAIQ，但还没有执行 `stream-start`，或者常驻服务和交互程序互相占用。按第 7 节停止服务后，只运行一个程序，再执行 `stream-start all`。

### 18.4 `capture-status` 中 frames 一直是 0

检查：

- 是否真的执行 `stream-start all`；
- `/dev/video22`、`/dev/video31` 是否被其他程序占用；
- 是否存在 `rkaiq_3A_server`；
- 启动日志是否有 MIPI、ISP 或 DQBUF 错误；
- media graph 的节点编号是否变化。

### 18.5 命令返回 OK，但帧率没有变化

一定用 20 秒前后 `frames` 差值计算真实 FPS。当前版本正是这种情况：命令返回 OK，但两路仍约 26.3 fps。需要检查 IMX586 VBLANK/帧长寄存器控制、驱动 V4L2 control 和 IQ `CISMinFps`。

### 18.6 HTTP 浏览器打不开

检查顺序：

1. `net-status all` 是否显示 running；
2. 板端 `curl http://127.0.0.1:8080/cam0` 是否有 HTTP 200；
3. 板端 `ip -br addr` 是否真的有电脑可达 IP；
4. 电脑 `ping 板卡IP` 是否通；
5. 浏览器使用的是板卡 IP，不是电脑的 `127.0.0.1`；
6. 防火墙是否允许 TCP 8080。

### 18.7 UVC 日志出现 `No such device (19)`

若它发生在电脑关闭 ffplay/v4l2-ctl 时，并且重新打开可恢复，这是主机释放流。若电脑仍在取流时持续出现，则检查 USB 线、Hub、供电、UDC 状态和是否发生重新枚举。

### 18.8 ISO 始终是 0

0 不是有效 ISO。先把“增益控制”和“ISO 计算/回读”分开验收。检查 RKAIQ 当前版本的曝光结果 API、模拟/数字/ISP 增益和 IQ 标定映射，修复后再验证 EXIF `PhotographicSensitivity`。

---

## 19. 测试结束后恢复现场

如果当前还在 `camera-aiq>` 提示符，输入：

```text
net-stop all
uvc-stop all
photo-stop 0
photo-stop 1
save-stop 0
save-stop 1
stream-stop all
quit
```

某项没有启动时返回错误可以忽略，重点是退出交互程序。然后在**板卡 shell**执行：

```bash
systemctl stop camera-http.service 2>/dev/null || true
systemctl enable camera-uvc.service
systemctl restart camera-uvc.service
sleep 5
systemctl is-enabled camera-uvc.service
systemctl is-active camera-uvc.service
systemctl show camera-uvc.service -p NRestarts -p ActiveState -p SubState
journalctl -u camera-uvc.service -n 100 --no-pager
```

最终通过条件：服务为 enabled/active，`NRestarts=0`，日志再次出现双相机、双 UVC 和 `/dev/ttyS9` READY。

本次测试结束后已经恢复该状态。

---

## 20. 本次证据文件

电脑端源码目录中的证据：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/
  test_results/full_acceptance_20260803/uvc/
    cam0_before.jpg
    cam1_before.jpg
    cam0_exp5000_gain1000.jpg
    cam0_exp30000_gain8000.jpg
    cam1_exp5000_gain1000.jpg
    cam1_exp30000_gain8000.jpg
    cam1_while_cam0_low.jpg
    cam1_while_cam0_high.jpg
    cam0_while_cam1_low.jpg
    cam0_while_cam1_high.jpg
    exposure_gain_comparison.jpg
    cam0_final.jpg
    cam1_final.jpg
```

对比图排列：左上 cam0 低曝光/低增益，右上 cam0 高曝光/高增益，左下 cam1 低曝光/低增益，右下 cam1 高曝光/高增益。

![两路曝光和增益实测对比](test_results/full_acceptance_20260803/uvc/exposure_gain_comparison.jpg)

最终恢复服务后的两路画面：

![camera 0 最终画面](test_results/full_acceptance_20260803/uvc/cam0_final.jpg)

![camera 1 最终画面](test_results/full_acceptance_20260803/uvc/cam1_final.jpg)

板卡端测试数据保留在：

```text
/root/camera_uart/test_results/full_acceptance_20260803
```

板端保存数据约数百 MB。确认不再需要取证后再人工清理，不要在测试脚本中自动删除。

---

## 21. 最终验收签字表

建议测试人员打印或复制此表逐项填写：

| 编号 | 项目 | 结果（通过/失败/未测） | 证据文件或日志 | 测试人/日期 |
| ---: | --- | --- | --- | --- |
| 1 | 双 IMX586 同时采集 |  |  |  |
| 2 | cam0/cam1 独立 IQ |  |  |  |
| 3 | 双路 4000x3000 UVC |  |  |  |
| 4 | 双路 UVC 各 1000 帧 |  |  |  |
| 5 | UVC 按路启停隔离 |  |  |  |
| 6 | cam0 独立曝光/增益 |  |  |  |
| 7 | cam1 独立曝光/增益 |  |  |  |
| 8 | cam0=4 Hz、cam1=2 Hz 实测 |  |  |  |
| 9 | ISO 有效非零回读 |  |  |  |
| 10 | 双路 eMMC 保存 |  |  |  |
| 11 | HTTP 板内输出 |  |  |  |
| 12 | HTTP 电脑直连 |  |  |  |
| 13 | UART 软件协议 |  |  |  |
| 14 | UART 实线命令和 ACK |  |  |  |
| 15 | 模拟 trigger/frame 绑定 |  |  |  |
| 16 | 真实 XVS 双相机同步 |  |  |  |
| 17 | PPS/GPRMC/UTC |  |  |  |
| 18 | JPEG/EXIF 字段 |  |  |  |
| 19 | 30～60 分钟长稳 |  |  |  |
| 20 | 整机功耗和温度 |  |  |  |
| 21 | 服务恢复和开机自启 |  |  |  |

只有失败项修复、所有必需的“未测”项目补齐并附上证据后，才能把整个需求标记为最终验收完成。
