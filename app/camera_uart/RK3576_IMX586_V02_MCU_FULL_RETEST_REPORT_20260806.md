# RK3576 双 IMX586 v0.2 + STM32 全面复测报告

报告版本：V1.0  
测试日期：2026-08-06  
测试依据：《RK3576 MIPI 相机模组任务计划 v0.2》  
测试对象：RK3576 + 两路 IMX586 + STM32F103 XVS，4000x3000  
板端程序：`/root/camera_uart/camera_aiq_test`  
本地证据目录：`test_results/20260806_mcu_full_retest`  
板端原始证据目录：`/root/camera_uart/test_results/20260806_mcu_full_retest`  

本报告只验收任务计划阶段 1～7。阶段 8 YOLO、阶段 9 全功能功耗优化和阶段 10
交付镜像不在本次范围内。

---

## 1. 最终结论

当前不能写成“阶段 1～7全部验收通过”。两路相机和主要软件功能已经工作，但 STM32
当前只提供固定 2 Hz XVS，UART 没有返回数据；PPS/GNSS、真实 UTC 和曝光起点相位也
没有硬件测量。准确结论如下。

| 阶段 | 任务 | 本次判定 | 核心结果 |
|---:|---|---|---|
| 1 | 双 IMX586 同时出图 | **实机通过** | 双路 4000x3000 连续采集，2 Hz 下均无 sequence 跳号 |
| 2 | 独立 ISP/IQ 和参数 | **控制实机通过；IQ 调校未完成** | 曝光、增益和 ISO 映射独立设置、独立回读；两份 IQ 路径独立但内容相同 |
| 3 | 2～4 Hz 和功耗 | **部分通过** | 双路真实 2/2 Hz 通过；4 Hz及四组合不通过；未接功耗仪 |
| 4 | 完整驱动和 UART 控制 | **软件通过；MCU 实线不通过** | RK3576 `/dev/ttyS9` 为115200、8N1，发送有计数；MCU返回0字节 |
| 5 | 硬件触发同步 | **2 Hz XVS 跟随通过；完整同步未通过** | 两路受同一 XVS 驱动，同序号 EOF时间差通常为微秒级；未测曝光起点相位，未接 PPS/GNSS |
| 6 | 曝光时间和 EXIF | **软件模拟通过；真实 UTC待验收** | 11对照片的 Trigger/frame_id/曝光/增益/ISO/CSV/EXIF绑定正确；`utc_valid=0` |
| 7 | UVC、网口、eMMC | **功能实机通过；USB3.0不通过** | 双 UVC、RNDIS ping/SSH、双 HTTP、双 eMMC短测通过；实际 USB速度只有480M |

### 1.1 已经真实通过的能力

- 两颗 IMX586 同时识别、同时经过独立 ISP节点输出4000x3000 NV12。
- STM32 的共享 2 Hz XVS 能驱动两颗处于 XVS从模式的 IMX586。
- 两路连续采集均达到约2.000 Hz，sequence连续，没有采集层丢帧。
- cam0/cam1 曝光、模拟增益和 ISO映射可以分别设置，修改一路不会改变另一路。
- 双路 UVC、双路 HTTP、RNDIS网络、SSH和eMMC短时保存可以共同工作。
- 停止 UVC后，RNDIS、ping、SSH和HTTP仍保持在线；重新启动UVC可以恢复出图。

### 1.2 尚未通过的关键能力

- STM32没有响应UART命令，现阶段只能判断它固定输出约2 Hz，不能通过RK3576切到4 Hz。
- 无法执行需求中的 `4/4、4/2、2/4、2/2` 四种真实 sensor帧率组合。
- 没有PPS和GPRMC/GNRMC，无法得到有效UTC；现有Trigger照片测试使用软件模拟源。
- 没有测两颗sensor真实曝光起点相位差。V4L2 EOF时间戳不是曝光起点时间戳。
- USB Gadget具备SuperSpeed能力，但本次电脑链路经过USB2 Hub，只枚举为480 Mbps。
- 未连接外部功耗仪，待机、单路、双路、UVC、网络和eMMC实际功耗没有数据。
- 两份 IQ 文件内容相同，尚未分别完成广角和鱼眼的 AWB、LSC、LDC等标定。

---

## 2. 判定规则

| 状态 | 含义 |
|---|---|
| 实机通过 | 本次真实硬件、真实图像或真实传输达到要求 |
| 软件模拟通过 | 程序逻辑由模拟 Trigger、伪串口或自检验证，不能代替外部硬件 |
| 部分通过 | 该阶段的一部分达到要求，仍有明确子项未完成 |
| 不通过 | 当前具备测试条件，但实测结果不满足目标 |
| 待外设验收 | 缺少PPS/GNSS、示波器、USB3链路或功耗仪，当前不能给硬件结论 |

以下现象不能单独作为通过依据：

- 命令返回 `OK`，但帧计数没有增长。
- `STATUS` 显示 `requested_fps_x1000=4000`，但 `CAPTURE_STATUS` 实测仍是2 Hz。
- UVC设备被电脑枚举，但电脑没有读取到有效4000x3000 JPEG帧。
- 软件 EOF时间戳相差几微秒，就直接认定曝光起点同步达到几微秒。
- 模拟 Trigger 能保存照片，就认定 MCU/PPS/GNSS硬件链已经通过。

---

## 3. 测试环境和设备映射

### 3.1 相机节点

| camera_id | sensor | ISP/NV12 | RAW10 | sensor subdev | AIQ params | IQ目录 |
|---:|---|---|---|---|---|---|
| 0 | `m00_b_imx586 4-001a` | `/dev/video22` | `/dev/video0` | `/dev/v4l-subdev4` | `/dev/video29` | `/etc/iqfiles/cam0` |
| 1 | `m01_b_imx586 5-001a` | `/dev/video31` | `/dev/video11` | `/dev/v4l-subdev9` | `/dev/video38` | `/etc/iqfiles/cam1` |

设备编号可能随内核和 media graph变化。重新刷机后必须先看程序的 `CAMERA_INIT` 和
`capture-status all`，不要永久假设节点编号不会变化。

### 3.2 串口、USB和网络

| 链路 | 参数 | 用途 |
|---|---|---|
| 电脑调试串口 | `/dev/ttyUSB0`，1500000 baud | 登录RK3576 Shell，不是MCU控制串口 |
| MCU UART | RK3576 `/dev/ttyS9`，115200、8N1、无流控 | RK3576与STM32命令/状态通信 |
| STM32 XVS | 共用一根FSYNC/XVS线，当前约2 Hz | 同时触发两颗IMX586 |
| RNDIS | 板端 `192.168.55.1/24`，电脑本次 `192.168.55.9/24` | ping、SSH、HTTP |
| HTTP | `http://192.168.55.1:8080/cam0` 和 `/cam1` | 两路MJPEG输出 |
| UVC | 电脑实际视频节点 `/dev/video0`、`/dev/video2` | 两路4000x3000 MJPEG |

### 3.3 软件版本

本地和板端 `camera_aiq_test` 的 SHA-256相同：

```text
c6cdd3064a1509ad2175590d2e2e68fe3016758fac0d656f296d0bb26c282a58
```

两份 IQ 文件的 SHA-256也相同：

```text
d0f4a039190140e17995441f72dd162c0157e85ddea808281d3010cb508bb027
```

第二个结果说明：程序已经使用独立目录和独立 RKAIQ context，但两路实际加载的调校内容
仍相同。它不能证明广角与鱼眼已经分别完成光学校正。

---

## 4. 测试前的正确启动方式

### 4.1 产品常驻模式

板卡正常运行由服务启动：

```bash
systemctl start camera-uvc.service
systemctl status camera-uvc.service --no-pager
```

当前服务配置为：

```text
ExecStart=/root/camera_uart/camera_aiq_test --all-daemon
Environment=UVC_CNT=2
Restart=always
```

本次收尾复核结果：

```text
ActiveState=active
SubState=running
MainPID=9118
NRestarts=0
usb0=192.168.55.1/24, UP
```

当前服务没有增加 `--uart /dev/ttyS9`。原因是程序启动时会先对 MCU执行握手，而目前
MCU无应答；强行带该参数会让统一UART初始化失败。

### 4.2 手工交互测试模式

应从调试串口登录板卡后执行：

```bash
systemctl stop camera-uvc.service
cd /root/camera_uart
./camera_aiq_test
```

然后看到 `camera-aiq>` 提示符再输入测试命令。

不要通过正在使用的RNDIS SSH会话停止整个服务，否则USB复合设备重建时SSH本身可能
断开。测试 `uvc-stop` 时必须在 `camera-aiq>` 中执行 `uvc-stop all`，不要用停止
systemd服务代替。

---

## 5. 阶段1：双IMX586同时出图

### 5.1 测试方法

程序控制台输入：

```text
stream-start all
wait 10000
status all
capture-status all
```

另外停止相机服务后，用RAW节点并发读取两路各60帧，核对sequence和时间戳。

### 5.2 实测结果

- 两颗传感器都识别为 IMX586 ID `0586`。
- 两路都进入 `XVS slave mode enabled: low-active, input-thin=1/1`。
- RAW测试两路均取得60帧，sequence均为0～59，无跳号。
- cam0稳态周期范围约500.005～500.039 ms，平均500.026 ms。
- cam1稳态周期范围约500.003～500.044 ms，平均500.026 ms。
- 对应帧率分别约1.999897 Hz和1.999898 Hz。
- 交互程序长时间运行检查时，两路均超过2185帧，`sequence_drops=0`。

### 5.3 判定

**实机通过。** 两路MIPI、DTS、media graph、ISP和4000x3000采集链已经同时工作；
两路也能持续跟随STM32当前2 Hz XVS。

启流瞬间内核曾偶发：

```text
MIPI_CSI2 ERR2:0xf0000
vblank need >= 1000us, cur 696us
```

稳定运行后没有形成持续错误风暴，也未导致sequence跳号，但后续仍应确认vblank设置，
并在30～60分钟正式长稳中统计启动后是否继续新增MIPI错误。

---

## 6. 阶段2：独立ISP/IQ与独立参数

### 6.1 测试命令

```text
exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
wait 5000
status all

exposure 0 10000
wait 3000
status all

gain 1 4000
wait 3000
status all

iso 0 150
iso 1 500
wait 5000
status all
```

### 6.2 实测结果

第一组设置后：

| 项目 | cam0 | cam1 |
|---|---:|---:|
| 请求曝光 | 5000 us | 20000 us |
| 回读曝光 | 约5004 us | 约19996 us |
| 请求增益 | 2.000x | 8.000x |
| 回读增益 | 2.000x | 8.000x |

隔离测试：

- 只把cam0曝光改为10000 us后，cam0回读约9998 us；cam1仍约19996 us、8x。
- 只把cam1增益改为4x后，cam0仍约9998 us、2x。
- `iso 0 150` 映射为cam0 3x增益。
- `iso 1 500` 映射为cam1 10x增益。
- 两路均返回 `manual_settings_verified=1`。

### 6.3 ISO字段为什么有两个

当前 RKAIQ查询结果中的 `aiq_iso=0`，因此程序无法直接取得RKAIQ原生ISO。程序使用
基础ISO 50和实际模拟增益计算：

```text
estimated_iso = 50 * actual_gain
```

所以状态中会看到 `iso_estimated=1`。例如3x对应ISO 150，10x对应ISO 500。
这条控制路径和回读逻辑已经通过，但它是工程映射值，不等于经过实验室标定的真实感光度。

### 6.4 判定

- 曝光、增益、ISO映射以及两路控制隔离：**实机通过**。
- 独立RKAIQ context和独立IQ路径：**实机通过**。
- 广角/鱼眼各自不同的IQ调校内容：**未完成**，因为当前两个JSON哈希相同。
- 白平衡、黑电平、LSC和LDC画质标定：本轮没有标定数据，不能判完成。

---

## 7. 阶段3：2～4 Hz、稳定性和功耗

### 7.1 当前2 Hz结果

当前STM32真实输出固定约2 Hz。两颗IMX586均为 `input-thin=1/1`，因此每个XVS脉冲
输出一帧。两路实测均约2.000 Hz，sequence无跳号。这一项通过。

### 7.2 4 Hz测试命令和失败结果

```text
fps 0 4
fps 1 4
wait 10000
capture-status all
```

程序报：

```text
ERROR command=fps ... reason="target fps did not stabilize"
```

帧周期仍约500 ms。即使状态里有 `requested_fps_x1000=4000`，真实采集仍为2 Hz。
这说明STM32没有切换到4 Hz，不能把请求值当成实测值。

### 7.3 为什么现在不能做四种组合

现有设计应该让STM32始终输出共享4 Hz基准：

- 某颗相机需要4 Hz时，`input-thin=1/1`，每个XVS都出帧。
- 某颗相机需要2 Hz时，`input-thin=1/2`，每两个XVS取一帧。

现在共享基准只有2 Hz。如果再给某颗相机设置二分频，它会变成约1 Hz，而不是目标2 Hz。
因此当前不能正式执行以下四种组合：

| 组合 | 当前结果 |
|---|---|
| cam0=4、cam1=4 | 不通过，两路仍约2 Hz |
| cam0=4、cam1=2 | 无4 Hz基准，无法正确验收 |
| cam0=2、cam1=4 | 无4 Hz基准，无法正确验收 |
| cam0=2、cam1=2 | 实际双路2 Hz通过，但不是在4 Hz基准下的独立分频验收 |

### 7.4 资源和温度

双相机、双HTTP和UVC待机时观测到：

- `camera_aiq_test` 约占58%单核CPU显示值。
- 系统总体约97.9% idle。
- 已用内存约811～815 MiB，可用约3.0 GiB，无swap。
- SoC约47.2摄氏度，大核/DDR约49.0摄氏度，小核/GPU约49.9摄氏度，NPU约47.2摄氏度。

没有外部功耗仪，因此待机、单路、双路采图和全输出功耗均为**待硬件验收**。

---

## 8. 阶段4：完整驱动和UART统一控制

### 8.1 软件控制接口

统一程序支持带 `camera_id` 的曝光、增益、ISO、帧率、保存、同步、UVC、网络和状态
命令。程序通过V4L2/RKAIQ及各输出后端控制相机，不从应用层直接裸写sensor寄存器。

### 8.2 真实MCU UART测试

确认 `/dev/ttyS9` 没有被其他进程占用，并配置为115200、8N1、无流控后，发送：

```text
$XVS,1,PING*2D89\r\n
$XVS,1,STATUS*CEDA\r\n
$XVS,3,START,4000,10*BE30\r\n
```

结果：

- PING的3秒接收窗口为0字节。
- STATUS/START的8秒接收窗口为0字节。
- `/proc/tty/driver/serial` 显示ttyS9 `tx:220 rx:0`。
- 两路XVS仍约2 Hz，没有变成4 Hz。

### 8.3 判定和故障范围

RK3576串口控制器已经把数据发送出去，但没有收到STM32任何返回。以下原因尚不能仅靠
RK3576软件区分：

- STM32固件只固定输出PWM/XVS，没有实现本项目UART协议。
- STM32 TX没有接到RK3576 RX，或RK3576 TX没有接到STM32 RX。
- TX/RX没有交叉、没有共地、使用了错误串口引脚。
- STM32使用的波特率不是115200，或串口参数不是8N1。
- 两端电平不一致；本链路应确认均为3.3 V TTL，不能直接接RS-232电平。

结论：RK3576 UART配置和软件协议自检通过；真实MCU实线双向通信**不通过**。

---

## 9. 阶段5：硬件触发同步

### 9.1 已通过的硬件部分

- 两颗IMX586都进入XVS从模式。
- 一根共享XVS线驱动两路，STM32当前持续提供约2 Hz脉冲。
- 双路各60帧、sequence 0～59、平均周期约500.026 ms。
- 长稳运行两路帧数保持一致，采集层 `sequence_drops=0`。
- 同一sequence的V4L2 EOF单调时间戳差通常约2～19 us，最近检查约9～10 us。

这可以证明：两路稳定跟随同一个XVS源，软件能够按同一帧号配对。

### 9.2 不能从软件时间戳得出的结论

V4L2时间戳位于ISP/V4L2帧完成链路，受到两路CSI、ISP调度和驱动排队影响。它不是
IMX586像素真正开始积分的曝光起点。因此“EOF差10 us”只能说明两路完成事件接近，
不能写成“曝光起点同步误差10 us”。

### 9.3 仍未完成的硬件同步验收

- 4 Hz共享XVS。
- 外部1 Hz、2 Hz、4 Hz三档触发全覆盖。
- PPS秒边沿与GPRMC/GNRMC UTC绑定。
- Trigger到frame event的固定响应延迟标定。
- 两颗IMX586真实曝光起点/曝光有效信号的相位差测量。

只有一个示波器通道时，可以分别相对同一XVS边沿测cam0和cam1，但两次采样会引入触发
抖动和不可重复误差。最终精确验收应使用至少双通道示波器或逻辑分析仪同时采集参考XVS
与两路可观测曝光/帧信号；如果板上没有两路可观测曝光信号，则需从sensor测试点、驱动
硬件时间戳或外接光电测量方案补充证据。

---

## 10. 阶段6：照片、frame_id、时间和EXIF

### 10.1 无PPS/GNSS条件下的软件模拟命令

```text
time-sync-reset
sync-bind-reset 1
sync-bind-log /root/camera_uart/test_results/20260806_mcu_full_retest/sync_bind.csv
photo-start 0 /root/camera_uart/test_results/20260806_mcu_full_retest/photos_cam0
photo-start 1 /root/camera_uart/test_results/20260806_mcu_full_retest/photos_cam1
sync-sim-start 2 12
wait 8000
sync-sim-status
sync-status
sync-bind-status
sync-bind-last
time-sync-status
photo-stop 0
photo-stop 1
```

### 10.2 实测结果

- 模拟器发出12次Trigger。
- 按配置忽略1次预触发，形成11个完整左右照片对。
- `trigger_id_gaps=0`、`duplicate_triggers=0`、`pending=0`。
- cam0和cam1各保存11张4000x3000 JPEG。
- 两路均无queue drop、无无效metadata、无编码/EXIF/写文件错误。
- `sync_bind.csv` 为12行，包括表头和11对记录。
- 两路 `stage6_metadata.csv` 各12行。
- 同一trigger的cam0/cam1 `frame_id` 一致，frame完成时间差约9～10 us。

实际JPEG中存在 `camera_id`、`frame_id`、`trigger_id`、`exposure_us`、
`gain_x1000`、`iso` 和 `trigger_time` 等UserComment信息。例如：

```text
cam0 frame_id=439 trigger_id=12 exposure_us=9998 gain_x1000=3000 iso=150
cam1 frame_id=439 trigger_id=12 exposure_us=19996 gain_x1000=10000 iso=500
```

### 10.3 时间真实性边界

照片明确标记：

```text
trigger_source=SIM
utc_valid=0
iso_estimated=1
```

板端当时系统日期为2026-06-06，与实际测试日期2026-08-06不一致。这进一步证明当前
没有有效外部UTC源。因此本次只能判Trigger/frame_id/照片/CSV/EXIF软件逻辑通过，
不能判PPS、GNSS UTC、真实MCU EVT和曝光响应offset已通过。

---

## 11. 阶段7：eMMC、UVC和网口输出

### 11.1 双eMMC短时保存

程序控制台输入：

```text
save-start 0 /root/camera_uart/test_results/20260806_mcu_full_retest/nv12_cam0
save-start 1 /root/camera_uart/test_results/20260806_mcu_full_retest/nv12_cam1
wait 2000
save-stop 0
save-stop 1
capture-status all
```

结果：

- 两路各保存3帧NV12。
- 每帧18,000,000 bytes，等于 `4000 * 3000 * 1.5`。
- 每路合计54,000,000 bytes。
- 两路 `save_queue_drops=0`、`save_failures=0`。

判定：双路eMMC短时保存实机通过。板端根分区约28 GiB，使用率约96%，只剩约1.1 GiB，
本轮没有继续做长时间原始NV12写盘，也没有做满盘和断电完整性测试。

### 11.2 双UVC电脑端测试

电脑先确认设备：

```bash
v4l2-ctl --list-devices
lsusb -t
```

本次UVC实际取流节点是 `/dev/video0` 和 `/dev/video2`。并发读取命令可用：

```bash
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 4000x3000 \
  -framerate 2 -i /dev/video0 -frames:v 6 cam0_uvc.mjpeg
```

另一个电脑终端同时执行：

```bash
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 4000x3000 \
  -framerate 2 -i /dev/video2 -frames:v 6 cam1_uvc.mjpeg
```

结果：

- 两路均读取6帧成功。
- `ffprobe`确认两路均为4000x3000、MJPEG、6帧。
- 稳定段帧间隔约499.9～500.0 ms。
- 板端两路均 `submitted=7 encoded=7 sent=7 encode_errors=0`。
- 主机启流最初发生sequence 0到3的启动跳变，稳定后连续；这属于主机UVC启流阶段丢2个
  传输包，不是相机采集层sequence丢帧。

主机停止读取后板端可能出现 `Unable to queue buffer: No such device`，这是电脑关闭UVC
节点后的收尾过程，不代表持续运行时编码失败。

### 11.3 USB速度

Gadget配置支持SuperSpeed，但本次连接经过USB2 Hub，电脑看到：

```text
Video + RNDIS ... 480M
```

所以双UVC功能通过，USB3.0链路不通过。更换真实USB3主机口、USB3 Hub和完整
SuperSpeed线后，`lsusb -t` 必须显示 `5000M` 或更高，才可判USB3通过。

### 11.4 RNDIS、ping和SSH

电脑端配置或确认本机RNDIS地址后执行：

```bash
ping -c 3 192.168.55.1
ssh root@192.168.55.1
```

结果：

- 电脑地址为 `192.168.55.9/24`。
- 板卡 `usb0` 为 `192.168.55.1/24` 且UP。
- ping 3/3成功，0%丢包。
- TCP 22端口可达，密码登录成功。

另一台电脑不能直接使用 `ssh -p 2222 root@10.100.2.67` 代替这条链路，除非那台电脑
也具有对应USB转发/端口映射环境。对本产品RNDIS直连的通用测试地址是
`root@192.168.55.1`。

### 11.5 双HTTP

板端程序中启动：

```text
net-start 0
net-start 1
net-status all
```

电脑浏览器分别访问：

```text
http://192.168.55.1:8080/cam0
http://192.168.55.1:8080/cam1
```

也可抓取单帧验证：

```bash
ffmpeg -y -i http://192.168.55.1:8080/cam0 -frames:v 1 cam0_http.jpg
ffmpeg -y -i http://192.168.55.1:8080/cam1 -frames:v 1 cam1_http.jpg
```

本次两路均取得有效4000x3000 JPEG。目视确认cam0是广角视角，cam1是明显鱼眼视角，
UVC与HTTP的同一camera_id画面对应一致。

### 11.6 UVC和网口生命周期隔离

程序运行时执行：

```text
uvc-stop all
```

程序返回 `usb_gadget=kept rndis=kept`。随后确认：

- RNDIS仍在线。
- ping和SSH仍可用。
- 两路HTTP继续出图。
- UVC停止出帧。

再执行：

```text
uvc-start all
```

电脑重新取得两路UVC画面。判定：UVC和RNDIS/HTTP生命周期隔离实机通过。

---

## 12. 给初次测试人员的完整复测顺序

以下顺序可以避免“设备已经枚举但没有真实帧”或“停止服务导致自己的SSH断开”等误判。

### 第一步：检查硬件

1. 两颗IMX586排线插紧，确认cam0和cam1都能读到sensor ID `0586`。
2. STM32 XVS、RK3576和相机板必须共地。
3. XVS应为空闲高、低脉冲有效；当前固件应先看到约500 ms周期。
4. UART应交叉连接：STM32 TX到RK3576 RX，STM32 RX到RK3576 TX。
5. Type-C使用数据线连接电脑；USB3验收必须使用SuperSpeed全功能链路。

### 第二步：检查常驻功能

在板卡调试串口执行：

```bash
systemctl is-active camera-uvc.service
systemctl show camera-uvc.service -p MainPID -p NRestarts --no-pager
ip -br addr show usb0
```

电脑执行：

```bash
ping -c 3 192.168.55.1
ssh root@192.168.55.1
v4l2-ctl --list-devices
lsusb -t
```

### 第三步：测试两路真实画面

1. 同时用 `/dev/video0` 和 `/dev/video2` 读取UVC帧。
2. 分别打开HTTP `/cam0` 和 `/cam1`。
3. 确认四张图不是黑图，分辨率均为4000x3000。
4. 确认cam0 UVC与cam0 HTTP是同一镜头，cam1同理。

### 第四步：进入交互程序测参数

从调试串口停止常驻服务并启动程序：

```bash
systemctl stop camera-uvc.service
cd /root/camera_uart
./camera_aiq_test
```

在 `camera-aiq>` 输入：

```text
stream-start all
wait 5000
capture-status all
```

确认两路 `frames` 持续增长、`fps_x1000` 接近2000、`sequence_drops=0`。

然后执行第6节的曝光、增益、ISO隔离命令，并检查 `manual_settings_verified=1`。

### 第五步：测试照片绑定和eMMC

没有PPS/GNSS时按第10节运行 `sync-sim-start`，结果只能标软件模拟通过。随后按第11.1节
每路保存3帧，核对每个NV12文件恰好18,000,000 bytes。

### 第六步：恢复产品状态

退出交互程序后，在调试串口执行：

```bash
systemctl start camera-uvc.service
systemctl is-active camera-uvc.service
```

最后重新检查ping、SSH、双UVC和双HTTP，避免测试结束后把板卡留在非产品状态。

---

## 13. MCU修复后的必测顺序

MCU侧确认UART固件和接线后，先不要直接跑全部测试。按以下顺序定位：

1. 在STM32 TX脚上确认发送波形，参数115200、8N1、3.3 V TTL。
2. RK3576发送PING，确认 `/proc/tty/driver/serial` 的tx增加。
3. 确认STM32收到PING并返回ACK，RK3576 ttyS9的rx开始增加。
4. 发送 `STATUS`，确认能读回当前XVS频率。
5. 发送 `START,4000,10`，实际测两路帧周期变为约250 ms，即4 Hz。
6. 只有4 Hz基准真实成立后，测试下表。

| 测试组合 | 程序命令 | 期望结果 |
|---|---|---|
| 4/4 | `fps 0 4`、`fps 1 4` | 两路均约250 ms/帧 |
| 4/2 | `fps 0 4`、`fps 1 2` | cam0约250 ms，cam1约500 ms |
| 2/4 | `fps 0 2`、`fps 1 4` | cam0约500 ms，cam1约250 ms |
| 2/2 | `fps 0 2`、`fps 1 2` | 两路均约500 ms/帧 |

每次组合至少运行60秒，记录 `frames` 增量、平均周期、最大周期、`sequence_drops` 和
内核MIPI错误。目标不是命令返回OK，而是 `CAPTURE_STATUS` 实测达到目标。

---

## 14. 当前问题的处理优先级

### P0：修复STM32 UART并输出共享4 Hz

这是阶段3和阶段4当前最直接的阻塞项。MCU固件需要实现项目协议，或至少先提供可确认的
4 Hz输出版本。修复后立即完成四种2/4 Hz组合。

### P1：接入PPS/GNSS和真实Trigger事件

接入后验证PPS、GPRMC/GNRMC、UTC锁定、MCU EVT、trigger_id连续性和frame_id绑定。
在此之前 `utc_valid=0` 是正确状态，不应由软件伪造为1。

### P1：标定真实曝光响应offset

通过至少双通道测量设备或sensor硬件时间戳，测得XVS/Trigger到真实曝光开始的固定延迟
和两路差异，再配置 `photo-offset`。未经标定时 `response_offset_ns=0` 不能作为最终值。

### P1：更换USB3链路

去掉USB2 Hub，使用电脑USB3控制器、USB3 Hub和SuperSpeed线。以 `lsusb -t` 显示
5000M或更高为准，再重新跑双UVC并发、长稳和端到端延迟测试。

### P2：完成两路独立IQ标定

在保留 `/etc/iqfiles/cam0` 和 `/etc/iqfiles/cam1` 独立目录的基础上，分别完成广角和鱼眼
的AE/AWB、黑电平、LSC、LDC和色彩标定，使两份JSON内容真正不同。

### P2：补功耗和存储可靠性

连接功耗仪测待机、单路、双路、同步、UVC、网络、eMMC等工况。清理磁盘后再做30～60
分钟输出长稳、满盘保护和异常断电完整性测试。

---

## 15. 证据索引

本地证据根目录：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/
  test_results/20260806_mcu_full_retest/
```

主要文件：

| 文件 | 内容 |
|---|---|
| `board/aiq_full.log` | 本轮完整交互程序日志 |
| `board/stage4_ttyS9_115200_8n1.txt` | MCU UART参数证据 |
| `board/stage4_serial_counters_after.txt` | ttyS9 TX/RX计数 |
| `board/stage4_mcu_ping_rx.bin` | PING接收，0字节 |
| `board/stage4_mcu_status_start_rx.bin` | STATUS/START接收，0字节 |
| `board/sync_bind.csv` | 11个完整Trigger/双帧绑定记录 |
| `board/photos_cam0`、`photos_cam1` | 两路各11张带EXIF JPEG及metadata CSV |
| `board/nv12_cam0`、`nv12_cam1` | 两路各3帧NV12及CSV |
| `host/rk3576_final_uvc_cam0.jpg` | 电脑端cam0 UVC实图 |
| `host/rk3576_final_uvc_cam1.jpg` | 电脑端cam1 UVC实图 |
| `host/rk3576_final_http_cam0.jpg` | 电脑端cam0 HTTP实图 |
| `host/rk3576_final_http_cam1.jpg` | 电脑端cam1 HTTP实图 |

相关专项记录：

- `RK3576_IMX586_STM32_XVS_2HZ_TEST_20260806.md`
- `MCU_XVS_UART_PROTOCOL.md`
- `RK3576_IMX586_V02_NO_MCU_SELF_TEST_GUIDE_20260805.md`

---

## 16. 一句话交付结论

当前版本已经实现双IMX586采集、独立曝光/增益/ISO控制、共享2 Hz XVS跟随、照片元数据
软件绑定、双UVC、双HTTP、RNDIS/SSH和双eMMC短时保存；尚未完成STM32 UART控制、
共享4 Hz和四种2/4 Hz组合、PPS/GNSS真实UTC、真实曝光起点同步误差、USB3.0以及功耗
验收，因此不能把阶段1～7整体标记为全部完成。
