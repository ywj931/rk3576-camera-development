# RK3576 双 IMX586 v0.2 全面测试报告与复测手册

测试日期：2026-08-03  
适用硬件：RK3576、两路 IMX586  
需求依据：`3576 MIPI 相机模组任务计划 v0.2`  
被测程序：`/root/camera_uart/camera_aiq_test`  
程序 SHA256：`dca521ab4a2acb3be0e332c099b1bc00451ae84b66d9b34f5c998aee840f185a`  
测试限制：本次没有连接 MCU、PPS、GPS/NMEA 和真实 Trigger；Type-C 实际只枚举为 USB High-Speed 480 Mbit/s。

---

## 1. 最终结论

当前版本已经实现并实测了两路 IMX586 同时采集、两路独立 IQ、独立增益控制、双路 JPEG/EXIF 落盘、双路 UVC 和双路 HTTP MJPEG 后端。程序的软件协议、自测同步和触发到照片绑定链路可运行。

但当前版本还不能判定为 v0.2 全部验收完成。以下项目没有达到需求：

1. `fps 0 4`、`fps 1 2` 下发成功，但两路实际都运行在约 10 fps，不是真正的 4 Hz 和 2 Hz。
2. 独立曝光命令返回成功，但实际曝光仍约为 30004 us，曝光设置未生效。
3. `status` 返回的 ISO 为 0；照片中写入的是由增益估算的 ISO，不是 RKAIQ 的真实 ISO。
4. USB UVC 双路可用，但链路是 480M USB 2.0 High-Speed，不是 USB 3.0 SuperSpeed。
5. HTTP 服务在板卡内部双路通过；电脑经 USB 虚拟网卡访问没有完成端到端验收。
6. `--daemon --uvc-daemon` 组合自动启动失败；手动共享一次采集后可同时启用 UVC 和 HTTP，但有少量损坏缓冲和输出队列丢弃。
7. 没有 MCU，因此真实 XVS、PPS、NMEA、Trigger、UTC 和 1000 次物理触发不能测试。
8. 采集启动时出现 MIPI CSI2 错误和 vblank 余量警告，停止时出现 RKAIQ 缓冲未释放警告。

一句话判断：**主要应用链路已经打通，双 UVC 和 eMMC 可用；低帧率、曝光、USB3、USB 网口复合链路和真实硬件同步仍需修复或补测。**

---

## 2. 本次测试结果总表

| 测试项 | 实测结果 | 判定 |
| --- | --- | --- |
| 两路 IMX586 识别 | cam0=`m00_b_imx586 4-001a`，cam1=`m01_b_imx586 5-001a` | 通过 |
| 两路独立 IQ | cam0、cam1 分别加载各自 `/etc/iqfiles` JSON | 通过 |
| 4000x3000 双路采集 | cam0 累计 4977 帧，cam1 累计 4741 帧 | 短时通过 |
| 采集零丢帧 | 30 fps 工况出现 sequence drop，且有 MIPI CSI2 ERR2 | 不通过 |
| 增益独立控制 | cam0=1x/8x、cam1=4x 均能回读并改变亮度，互不串扰 | 通过 |
| 曝光独立控制 | 请求 5000/20000 us 后仍回读约 30004 us | 不通过 |
| ISO 回读 | `status` 中 ISO=0；JPEG 元数据中的 ISO 为估算值 | 不通过 |
| 帧率 2 Hz/4 Hz | 请求 cam0=4、cam1=2，实测两路均约 10 fps | 不通过 |
| UART 协议解析 | 14 个 CAM 协议自测用例通过 | 通过 |
| `/dev/ttyS9` 配置 | 可配置为 115200、8N1 | 通过 |
| UART 真实双向通信 | 无 MCU/外部 UART 对端 | 未测试 |
| XVS 协议自测 | `XVS_PROTOCOL_SELF_TEST_OK` | 通过 |
| 触发绑定自测 | 2 个触发、2 个完整左右帧对 | 通过 |
| 2 Hz 软件模拟 | 10/10 完整左右帧对 | 软件通过 |
| 4 Hz 软件模拟 | 20/20 完整左右帧对 | 软件通过 |
| 真实硬件同步 | `physical_xvs=0`，没有 MCU/PPS/NMEA/Trigger | 未测试 |
| EXIF 软件自测 | APP1/EXIF、UTC 字段和 UserComment 结构通过 | 通过 |
| eMMC 双路保存 | 两路各 2 张 4000x3000 JPEG、CSV 写入错误为 0 | 通过 |
| 双 UVC | 两路同时各取 50 帧成功，均为 4000x3000 MJPEG | 通过 |
| USB 3.0 | UDC `current_speed` 和 `maximum_speed` 都是 `high-speed` | 不通过 |
| HTTP 双流后端 | 板内两路 HTTP 200；4 秒收到约 34.1 MB/30.6 MB | 板内通过 |
| 电脑访问 HTTP | RNDIS 枚举过，但电脑端未获得可用 IP；ECM 内核功能未开启 | 不通过 |
| UVC+HTTP 同时输出 | 手动流程可运行；自动组合启动失败，压力下有少量坏帧/队列丢弃 | 部分通过 |
| 双 UVC 服务资源快照 | `camera_aiq_test` 约 18.2% CPU、313568 KiB RES；系统可用内存约 3.1 GiB | 记录值 |
| 系统温度 | SoC 49.9、CPU 51.8至52.7、DDR 51.8 摄氏度 | 记录值 |
| 30至60分钟长稳、功耗 | 本次未执行 | 未测试 |
| YOLO | 不在本次相机程序回归范围 | 未测试 |

“软件通过”只表示程序逻辑正确，不表示传感器已经被同一个硬件边沿同步曝光。

---

## 3. 固定设备关系

| 名称 | camera_id | Sensor | IQ 文件 | ISP 参数节点 | NV12M 采集节点 |
| --- | ---: | --- | --- | --- | --- |
| 左/0 号相机 | 0 | `m00_b_imx586 4-001a` | `/etc/iqfiles/cam0/imx586_default_default.json` | `/dev/video29` | `/dev/video22` |
| 右/1 号相机 | 1 | `m01_b_imx586 5-001a` | `/etc/iqfiles/cam1/imx586_default_default.json` | `/dev/video38` | `/dev/video31` |

电脑端本次 UVC 图像节点是 `/dev/video0` 和 `/dev/video2`，`/dev/video1` 和 `/dev/video3` 是 metadata 节点。重新插拔后编号可能变化，必须重新枚举。

串口不要混淆：

| 用途 | 节点 | 参数 |
| --- | --- | --- |
| 电脑登录板卡的调试串口 | 电脑端 `/dev/ttyUSB0` | 1500000 baud |
| 相机控制、MCU 同步串口 | 板卡端 `/dev/ttyS9` | 115200、8N1 |

---

## 4. 小白复测前准备

### 4.1 硬件连接

1. 给 RK3576 使用独立电源供电。
2. 确认两颗 IMX586 已接好 MIPI 和供电。
3. 调试串口接电脑，用于登录板卡。
4. Type-C OTG 口通过数据线接电脑，用于 UVC 或 USB 虚拟网卡。
5. 测真实同步时再接 MCU、PPS、NMEA 和 Trigger。本次没有这些硬件。

Type-C 在 UVC 测试中是把板卡变成“USB 摄像头”，不是在 Type-C 上再接一颗摄像头。

### 4.2 电脑端安装工具

在**电脑端**执行：

```bash
sudo apt update
sudo apt install -y picocom v4l-utils ffmpeg usbutils curl exiftool
```

### 4.3 登录板卡

在**电脑端**执行：

```bash
picocom -b 1500000 /dev/ttyUSB0
```

出现登录提示后输入 `root`。退出 picocom：先按 `Ctrl+A`，再按 `Ctrl+X`。

### 4.4 避免程序冲突

`camera-uvc.service` 和手动运行的 `camera_aiq_test` 都会占用相机。做交互测试前，在**板卡端**执行：

```bash
systemctl stop camera-uvc.service
pkill rkaiq_3A_server 2>/dev/null || true
```

测试结束后必须恢复：

```bash
systemctl restart camera-uvc.service
systemctl is-active camera-uvc.service
```

预期最后输出 `active`。

---

## 5. 基线检查

以下命令均在**板卡端**执行。

```bash
sha256sum /root/camera_uart/camera_aiq_test
ls -lh /etc/iqfiles/cam0/imx586_default_default.json
ls -lh /etc/iqfiles/cam1/imx586_default_default.json
media-ctl -p | grep -i imx586
date -Ins
```

通过条件：

- 程序哈希与文档开头一致；
- 两个 IQ 文件都存在；
- 能看到两颗不同的 IMX586；
- 系统时间正确。

本次板卡时间比电脑时间约慢两个月，所以真实 UTC/EXIF 绝对时间不能判定通过。接 MCU/GPS 前先解决系统授时。

---

## 6. 软件自测

在**板卡端**停止 UVC 服务后执行：

```bash
cd /root/camera_uart

./camera_aiq_test --control-uart-protocol-self-test
./camera_aiq_test --sync-protocol-self-test
./camera_aiq_test --sync-bind-self-test
./camera_aiq_test --photo-exif-self-test
```

本次结果：

```text
CONTROL_UART_PROTOCOL_SELF_TEST_OK cases=14
XVS_PROTOCOL_SELF_TEST_OK
TRIGGER_BIND_SELF_TEST_OK triggers=2 pairs=2
PHOTO_EXIF_SELF_TEST_OK
```

这四项都不需要打开相机，也不需要 MCU。它们验证的是协议解析、数据结构和软件绑定算法。

---

## 7. 双路采集和独立 IQ

### 7.1 启动程序

在**板卡端**执行：

```bash
cd /root/camera_uart
./camera_aiq_test
```

看到 `camera-aiq>` 后依次输入：

```text
stream-start all
wait 5000
status all
capture-status all
```

通过条件：

- 两路都显示 `state=STARTED`；
- cam0 使用 `/dev/video22`，cam1 使用 `/dev/video31`；
- 日志中 cam0 使用 cam0 IQ，cam1 使用 cam1 IQ；
- 等待前后 `frames` 持续增加。

为什么必须先 `stream-start all`：RKAIQ `prepare` 只完成配置；真正的 sensor/ISP 数据流在 STREAMON 后才运行。曝光、帧率状态和 UVC/HTTP 都依赖持续到达的帧。程序当前有意要求先明确启动采集，再打开输出后端。

### 7.2 本次长帧计数结果

```text
cam0 frames=4977
cam1 frames=4741
```

两路都超过 1000 帧，证明双路能持续取帧。但序列统计有丢帧，而且内核出现：

```text
MIPI_CSI2 ERR2:0xf0000
vblank need >=1000us, cur 696us
```

因此“能连续采集”通过，“严格无 MIPI 错误、无丢帧”不通过。

---

## 8. 曝光、增益和帧率测试

保持 `camera_aiq_test` 和两路采集运行，在**板卡交互界面**输入。

### 8.1 增益独立控制

```text
status all
gain 0 1000
wait 2000
status all
gain 0 8000
wait 2000
status all
gain 1 4000
wait 2000
status all
```

参数单位为 `gain_x1000`：1000=1 倍，4000=4 倍，8000=8 倍。

本次结果：目标相机能回读到 1x、8x 或 4x，画面亮度随之变化；另一相机设置不被串改。独立增益通过。

### 8.2 曝光控制

```text
exposure 0 5000
exposure 1 20000
wait 2000
status all
```

期望：cam0 约 5000 us，cam1 约 20000 us。  
实测：两路仍约 30004 us。命令只返回 RKAIQ API 调用成功，但传感器实际值没有改变，因此判定失败。

### 8.3 2 Hz/4 Hz 帧率

```text
fps 0 4
fps 1 2
wait 10000
capture-status all
```

判断方法：记录等待前后帧数，新增帧数除以 10 秒。  
实测：两路 10 秒都新增约 100 帧，即约 10 fps，不是 4 fps 和 2 fps。

软件 `sync-sim-start 2/4` 能控制“每秒保存几张触发照片”，但不等于 sensor 连续采集已经降到 2/4 Hz。若需求是传感器随外部边沿曝光，必须使用 MCU/XVS/FSIN 从模式，并在示波器和帧事件上验证。

---

## 9. 无 MCU 条件下的同步测试

### 9.1 2 Hz 软件模拟

```text
sync-bind-reset 0
sync-sim-start 2 10
wait 6000
sync-bind-status
sync-status
```

本次结果：10 个触发全部形成左右完整帧对。

### 9.2 4 Hz 软件模拟

```text
sync-bind-reset 0
sync-sim-start 4 20
wait 6000
sync-bind-status
sync-status
```

本次结果：20 个触发全部形成左右完整帧对。

状态会明确显示：

```text
source=SIM
physical_xvs=0
utc_valid=0
TIME_SYNC UNLOCKED
diagnostic_only=1
```

本次不同运行中左右帧时间差出现约 1.94 ms 和 32.72 ms。因为它只是把连续采集帧按最近时间匹配，不能作为硬件同步精度。

### 9.3 接入 MCU 后必须补测

1. `/dev/ttyS9` 与 MCU 使用 115200、8N1，并确认电平匹配。
2. 验证 PING/ACK、IDLE、2 Hz、4 Hz 和 STOP 双向命令。
3. 用示波器同时测两路 XVS/FSIN，确认同一个边沿到达两颗 IMX586。
4. 输入 PPS 和 GPRMC/NMEA，确认 UTC 锁定。
5. 连续测试至少 1000 个真实 Trigger。
6. 每个 trigger_id 必须绑定 cam0、cam1 各一个 frame_id，无重复、无缺失。
7. 统计 trigger 到 frame event 延迟、左右曝光起始差和时间戳完整率。

没有完成这些步骤前，报告只能写“软件模拟通过，硬件同步未测试”。

---

## 10. eMMC 双路照片与 EXIF

在**板卡交互界面**输入：

```text
stream-start all
photo-start 0 /root/camera_uart/test_output/cam0
photo-start 1 /root/camera_uart/test_output/cam1
sync-bind-reset 0
sync-bind-log /root/camera_uart/test_output/sync.csv
sync-sim-start 2 10
wait 6000
photo-status all
sync-bind-status
photo-stop 0
photo-stop 1
```

在**板卡 Linux 命令行**检查：

```bash
find /root/camera_uart/test_output -type f -maxdepth 2 -ls
sed -n '1,5p' /root/camera_uart/test_output/cam0/stage6_metadata.csv
sed -n '1,5p' /root/camera_uart/test_output/cam1/stage6_metadata.csv
df -h /root
```

本次实测目录为：

```text
/root/camera_uart/test_output/v02_20260803/cam0
/root/camera_uart/test_output/v02_20260803/cam1
/root/camera_uart/test_output/v02_20260803/sync.csv
```

两路各保存 2 张 4000x3000 JPEG，`encode_errors=0`、`exif_errors=0`、`write_errors=0`。根文件系统可用空间约 10 GB。

注意：本次 `utc_valid=0`，`exposure_source=RKAIQ_LATEST_NOT_FRAME_BOUND`，ISO 也标记为 `iso_estimated=1`。文件和字段存在不代表真实 UTC、逐帧曝光参数已经验收通过。

---

## 11. 双路 UVC 测试

### 11.1 板卡端启动

在**板卡端**执行：

```bash
systemctl restart camera-uvc.service
systemctl status camera-uvc.service --no-pager
cat /sys/class/udc/23000000.usb/state
cat /sys/class/udc/23000000.usb/current_speed
cat /sys/class/udc/23000000.usb/maximum_speed
```

本次状态：服务为 `active`，UDC 为 `configured`，但两个 speed 都是 `high-speed`。

### 11.2 电脑端找 UVC 节点

在**电脑端**执行：

```bash
lsusb
lsusb -t
v4l2-ctl --list-devices
for n in /dev/video*; do
  echo "===== $n ====="
  v4l2-ctl -d "$n" --list-formats-ext 2>/dev/null
done
```

找到支持 `MJPG 4000x3000` 的两个图像节点。本次是 `/dev/video0` 和 `/dev/video2`。

### 11.3 同时抓取两路

在**电脑端开两个终端**，分别执行：

```bash
ffmpeg -hide_banner -f v4l2 -input_format mjpeg \
  -video_size 4000x3000 -framerate 10 -i /dev/video0 \
  -frames:v 50 -c copy cam0_50frames.mjpeg
```

```bash
ffmpeg -hide_banner -f v4l2 -input_format mjpeg \
  -video_size 4000x3000 -framerate 10 -i /dev/video2 \
  -frames:v 50 -c copy cam1_50frames.mjpeg
```

从两个流各取一张图：

```bash
ffmpeg -y -i cam0_50frames.mjpeg -frames:v 1 cam0.jpg
ffmpeg -y -i cam1_50frames.mjpeg -frames:v 1 cam1.jpg
file cam0.jpg cam1.jpg
```

本次两路各 50 帧都成功，样图均为 4000x3000，且画面内容不同，证明不是同一路复制。

### 11.4 为什么 USB3.0 不通过

电脑 `lsusb -t` 显示 RK3576 在 `480M` 分支；板卡 UDC 的 `maximum_speed` 也显示 `high-speed`。即使电脑和 Hub 有 SuperSpeed 口，本次 RK3576 这条实际链路仍是 USB2.0。

USB3.0 复测必须同时满足：

- 板卡 UDC `maximum_speed` 是 `super-speed`；
- 数据线和 Hub 支持 USB3；
- 电脑 `lsusb -t` 显示 `5000M` 或更高；
- 双路抓流稳定且无带宽错误。

---

## 12. HTTP 网口输出测试

程序发布地址：

```text
http://<板卡IP>:8080/
http://<板卡IP>:8080/cam0
http://<板卡IP>:8080/cam1
```

### 12.1 先验证板卡内部 HTTP 后端

在**板卡端**停止 UVC 服务，然后执行：

```bash
systemctl stop camera-uvc.service
cd /root/camera_uart
./camera_aiq_test --daemon
```

另开一个**板卡端终端**执行：

```bash
curl -I http://127.0.0.1:8080/
curl --max-time 4 -o /dev/null -w 'cam0 http=%{http_code} bytes=%{size_download}\n' \
  http://127.0.0.1:8080/cam0
curl --max-time 4 -o /dev/null -w 'cam1 http=%{http_code} bytes=%{size_download}\n' \
  http://127.0.0.1:8080/cam1
```

持续 MJPEG 被 `--max-time` 结束时，curl 返回超时码是正常的；关键是 HTTP=200 且 bytes 持续增加。

本次板内实测：cam0 约 34,077,001 bytes，cam1 约 30,639,128 bytes，两个流都以 multipart JPEG 开始，后端通过。

### 12.2 再验证电脑到板卡的网络

电脑和板卡必须先有真正可达的 IP。若使用独立以太网，在**电脑端**执行：

```bash
ping -c 4 <板卡IP>
curl -I http://<板卡IP>:8080/
```

然后用浏览器打开：

```text
http://<板卡IP>:8080/
```

不能把 `http://127.0.0.1:8080/` 直接输入电脑浏览器；电脑上的 127.0.0.1 指电脑自己，不是板卡。

### 12.3 本次 USB 虚拟网卡结果

RNDIS 能枚举为电脑网卡，板端 `usb0` 配置过 `10.200.2.67/24`，但电脑 NetworkManager 将接口标为 disconnected，未得到可用 IPv4。当前内核没有开启 `CONFIG_USB_CONFIGFS_ECM`，因此 ECM 复合方案也无法直接使用。

所以本次只能判定“HTTP 应用后端通过”，不能判定“电脑经 USB 网口访问通过”。

---

## 13. 同一 Type-C 同时 UVC 和网口

原理上可以：USB Gadget 一个配置中同时建立两个 UVC function 和一个 ECM/NCM/RNDIS 网络 function。电脑会同时看到两台 USB 摄像头和一张 USB 网卡。

当前版本的限制：

1. 当前链路仅 480M，双路 4000x3000 MJPEG 已经占用大量带宽。
2. ECM 未编入内核；RNDIS 的电脑端 IP 自动配置没有打通。
3. 同时运行 `--daemon --uvc-daemon` 时，HTTP 已启动 capture，UVC 自动启动再次调用 capture start，收到 `capture stream is already running` 后退出。

手动验证方法是在**板卡端**只启动一次采集：

```text
stream-start all
net-start 0
net-start 1
uvc-start all
net-status all
uvc-status all
capture-status all
```

本次手动模式下，电脑同时取得两路各 30 个 UVC 帧，板端 HTTP 编码计数也持续增加；但 UVC 报过 1/2 个损坏缓冲，HTTP 队列出现丢弃。因此判定为部分通过，不适合作为最终交付方式。

建议最终方案：启用 USB3 SuperSpeed；内核启用 ECM 或 NCM；gadget 固定为 `uvc.0 + uvc.1 + ecm/ncm`；程序只启动一次 capture，让 UVC、HTTP、eMMC 共享帧回调和引用计数。

---

## 14. UART 控制测试

### 14.1 无 MCU 时能测什么

在**板卡端**执行：

```bash
stty -F /dev/ttyS9 115200 cs8 -cstopb -parenb -ixon -ixoff -echo raw
stty -F /dev/ttyS9 -a | head -2
./camera_aiq_test --control-uart-protocol-self-test
```

本次确认速度为 115200，格式为 8N1，14 个协议用例通过。

### 14.2 为什么不能启动完整 MCU 模式

完整命令是：

```bash
./camera_aiq_test --uvc-daemon --uart /dev/ttyS9 --sync-timer-hz 1000000
```

`--uart` 会在启动时向 MCU 发 PING 并等待 ACK。没有 MCU 时失败是正确行为，不是相机故障。

### 14.3 接 MCU 后的验收顺序

1. 先只测 PING/ACK 和双向文本帧。
2. 再测 `STATUS`、`GAIN`、`EXPOSURE`、`FPS`。
3. 再测 `SAVE_START/STOP`、`UVC_START/STOP`、`NET_START/STOP`。
4. 最后接入 PPS/NMEA/Trigger，并做 1000 次物理同步测试。

UART 程序属于应用层控制程序。它通过 V4L2、RKAIQ 和保存/输出后端控制相机，不应通过 UART 命令直接裸写 IMX586 寄存器。

---

## 15. 当前问题原因与修改建议

### 15.1 资源快照说明

双 UVC 服务运行时采集了一次瞬时状态：系统 load average 为 `0.18/0.12/0.15`，CPU 总体约 97.8% idle，`camera_aiq_test` 在该次 `top` 采样中约占 18.2% CPU、7.9% 内存，常驻集约 313568 KiB。系统总内存 3.8 GiB，可用约 3.1 GiB；SoC 约 49.9 摄氏度，大小核、DDR、GPU 温度约 51.8至52.7 摄氏度。

这只是一帧资源快照，不是平均值、峰值或功耗结论。系统没有暴露可直接读取的 devfreq 目录，本次也没有外置功率计，因此 DDR 带宽、ISP 占用和功耗仍标为未测试。

### P0：2 Hz/4 Hz 没有真正生效

当前代码调用 `rk_aiq_uapi2_setFrameRate()`，API 返回成功后就把请求当作成功。实测传感器/ISP 最终回读 10 fps，说明 4000x3000 当前模式、vblank 范围或 RKAIQ 对低帧率进行了限制。

修复要求：

- 明确需求是“sensor 连续流 2/4 fps”还是“sensor 连续运行、每秒输出 2/4 张照片”；
- 若要连续流低帧率，在 IMX586 驱动中实现对应 frame interval/vblank 范围；
- 若要硬件触发，完成 XVS/FSIN 从模式，由 MCU 2/4 Hz 触发；
- 命令下发后必须等待数帧并回读，实际值不等于请求值时返回错误，不能打印 OK。

### P0：曝光命令没有真正生效

程序已经切到手动模式并调用手动曝光 API，但实测回读仍为 30004 us。需要结合本版本 RKAIQ v6 API 和 sensor controls 确认手动 AE 属性的正确写法，并检查帧长对曝光上限的约束。

同样必须增加“设置后回读验证”：只有实际曝光进入允许误差范围才返回成功。

### P0：USB3 没有建立

当前 UDC 自己报告 `maximum_speed=high-speed`，问题不只在电脑命令。需检查 RK3576 DWC3、USB3 PHY、Type-C lane/role、DTS `maximum-speed`、线缆和 Hub 路径。修复前不能把 480M UVC 称为 USB3 UVC。

### P0：USB 网口端到端未完成

优先启用并测试 `CONFIG_USB_CONFIGFS_ECM=y` 或 NCM，而不是依赖兼容性较差的 RNDIS；配置固定板端 IP 和 DHCP，先通过 Ping，再测试 HTTP。复合 gadget 必须保留双 UVC function。

### P1：UVC+HTTP 自动启动冲突

当前两个 autostart 函数都会启动 capture。应把采集启动放到统一入口，只执行一次；各输出后端只注册/启停自己的消费者，或给 capture 增加幂等/引用计数。

### P1：ISO、逐帧曝光和退出警告

- ISO 应从 RKAIQ 有效查询结果取得，并标明实际值还是估算值；
- 照片元数据应绑定对应帧的曝光参数，而不是 `RKAIQ_LATEST_NOT_FRAME_BOUND`；
- 退出前等待回调和编码队列清空，再停 RKAIQ，消除 `pool items are still in use`；
- 排查 MIPI `ERR2` 和 vblank 696 us 警告，完成无错误长稳测试。

---

## 16. 最终验收清单

交付前逐项打勾：

- [ ] 两路 4000x3000 连续运行 30至60 分钟，无 MIPI 错误、无不可解释丢帧。
- [x] 两路分别加载 cam0/cam1 IQ。
- [x] 两路增益可独立设置且互不串扰。
- [ ] 两路曝光可独立设置并正确回读。
- [ ] 2 Hz/4 Hz 实际帧率或真实外部触发达到需求。
- [ ] ISO 返回真实有效值，不是 0，也不是未标注的估算值。
- [x] 双路 UVC 4000x3000 能同时出图。
- [ ] 电脑确认 USB SuperSpeed 5000M 或更高。
- [x] HTTP 双路编码和服务在板内可运行。
- [ ] 电脑通过实际网口或 USB 网卡直接打开双路 HTTP。
- [x] 两路 JPEG、EXIF 和 CSV 可保存到 eMMC。
- [ ] UART 与真实 MCU 双向命令和 ACK 通过。
- [ ] PPS、NMEA、Trigger、真实 UTC 锁定通过。
- [ ] 1000 次物理触发全部形成完整左右帧对。
- [ ] UVC、网口、eMMC 同时运行的长稳和带宽测试通过。
- [ ] 记录 CPU、DDR、ISP、温度和外置功率计数据。

---

## 17. 测试结束后的板卡状态

本次结束时已经恢复双路 UVC 配置：

```text
camera-uvc.service = active (running)
USB gadget = uvc.0 + uvc.1
UDC state = configured
current_speed = high-speed
maximum_speed = high-speed
```

并在恢复服务后，再次同时抓取两路各 10 帧，两个 ffmpeg 进程都以 0 退出。

本次电脑端证据目录：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/test_results/20260803_v02_full_retest
```

其中包含双路 50 帧 UVC 流、双后端并发流、恢复服务后的双路流和对应样图。
