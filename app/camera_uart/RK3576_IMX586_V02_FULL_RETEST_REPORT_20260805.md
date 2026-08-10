# RK3576 双 IMX586 v0.2 全面复测报告

报告版本：V1.3  
测试日期：2026-08-05  
测试依据：《RK3576 MIPI 相机模组任务计划 v0.2》  
测试对象：RK3576 + 两路 IMX586，4000x3000  
板端程序：`/root/camera_uart/camera_aiq_test`  
证据目录：`test_results/20260805_v02_full_retest/run2`、`run3`  

本次 V1.3 补充了 run3 的 30 分钟并发长稳、UVC/RNDIS 生命周期和最终常驻服务冒烟。
程序、内核和设备树未在本次文档更新中修改。

---

## 1. 先看结论

本次已对任务计划“二、任务拆解与参考技术路径”中的阶段 1～7逐项进行真实测试，
并按照任务计划第四部分补测双路 UVC、双路 HTTP、RAW10、eMMC 保存、USB/RNDIS、
资源温度和四种 2/4 Hz 组合。

不能把当前版本写成“阶段 1～7全部验收通过”。准确结论如下：

| 阶段 | 任务 | 本次结论 | 结论摘要 |
|---:|---|---|---|
| 1 | 双 IMX586 同时出图 | **实机通过** | 双 sensor、双 ISP、双 RAW10、双 UVC 均取得有效图像 |
| 2 | 独立 ISP/IQ和参数 | **实机通过** | 两路曝光、增益、ISO映射独立控制和回读正确；两份 IQ 文件内容目前相同，RKAIQ 原生 ISO 回读为 0 |
| 3 | 双路帧率和功耗基线 | **不通过** | 真实 sensor `4/4`、`4/2`、`2/4`、`2/2` 未达到目标；功耗为待硬件验收项 |
| 4 | UART 统一控制 | **软件通过；实线待硬件验收** | 115200、8N1、14个协议用例及 ACK/NACK 软件通过；未接 MCU，`/dev/ttyS9` 实线未验收 |
| 5 | 硬件触发同步 | **软件通过；同步待硬件验收** | 模拟 Trigger 与左右 frame_id 绑定通过；当前没有 XVS 输入，不能判硬件同步通过 |
| 6 | 时间、曝光和 EXIF | **软件通过；UTC待硬件验收** | JPEG、EXIF、CSV和 frame_id绑定通过；没有 PPS/GNSS，真实 UTC和曝光相位未验收 |
| 7 | USB/网口/eMMC | **USB2.0功能实机通过；USB3.0和零丢帧不通过** | 双 UVC、双 HTTP、RNDIS、SSH和 eMMC 能工作；USB只有480 Mbps，高输入帧率下 eMMC/UVC均有丢帧 |

### 1.1 当前最重要的未通过项

1. 两颗 sensor 当前运行的是**自由运行 DTS**，没有启用 `sony,xvs-slave-mode`。
2. `fps 0/1 2/4` 走的是 XVS 输入抽帧控制；自由运行模式下驱动拒绝该 ioctl。
3. 所以四种 sensor 帧率组合实际仍约 22～26 fps，而不是目标 2/4 Hz。
4. UVC 端点按 2 fps或4 fps取流成功，只说明输出端节流成功，不说明 sensor 已切换为2/4 Hz。
5. 未接 MCU、XVS、PPS/GNSS和功耗仪，相关项目不能判通过。

### 1.2 本轮新增生命周期结论

在交互程序中执行 `uvc-stop all` 后，程序返回
`usb_gadget=kept rndis=kept`。实测 UDC 仍为 `configured`，电脑端 USB 仍为
`Bus 001 Device 012: ID 2207:0017`，`usb0`、ping、SSH 和两个 HTTP 流均保持在线；
两路 UVC 均停止出帧。随后执行 `uvc-start all`，电脑端重新取得 cam0 8 帧/4 fps 和
cam1 4 帧/2 fps，HTTP、ping 和 SSH 端口继续正常。最后重启
`camera-uvc.service`，服务恢复为 `active`，UDC 为 `configured`，双路 UVC/HTTP
冒烟再次通过。

### 1.3 对旧自测手册的纠正

`RK3576_IMX586_V02_SELF_TEST_GUIDE_20260804.md` 开头写“当前两颗 IMX586 已配置为
XVS 从模式”。本次读取实际运行配置和真实帧计数后确认，这句话与当前板卡不一致：

- 当前两路都处于自由运行状态；
- 当前 DTS 中没有 `sony,xvs-slave-mode` 和 `sony,xvs-input-thin`；
- 未接 MCU 时两路仍能以约 24 fps连续出图；
- 因此本报告以本次实机状态为准。

---

## 2. 判定规则

| 状态 | 含义 |
|---|---|
| 实机通过 | 本次有真实硬件、真实帧或真实输出证据，并满足判定值 |
| 软件通过 | 模拟器、伪终端或程序自检通过，不能替代外部硬件 |
| 待硬件验收 | 缺少 MCU、XVS、PPS/GNSS、USB 3.x直连或测量仪器，当前不能形成硬件结论 |
| 不通过 | 具备当前测试条件，但实测值不满足要求 |

以下情况不能单独作为“通过”依据：

- 命令只返回 `OK`，但帧计数没有增长；
- `STATUS requested_fps_x1000=4000`，但 `CAPTURE_STATUS` 实测不是4 Hz；
- UVC设备已经枚举，但电脑端没有取得有效 JPEG帧；
- HTTP首页能打开，但 `/cam0`、`/cam1` 没有图像数据；
- 软件模拟 Trigger 成功，但示波器上没有真实 XVS波形。

---

## 3. 测试环境和设备对应关系

### 3.1 三种命令执行位置

| 标记 | 执行位置 | 提示符示例 |
|---|---|---|
| 电脑端 | Ubuntu电脑 | `ywj@ywj:~$` |
| 板卡 Shell | RK3576 Linux | `root@localhost:~#` |
| 程序控制台 | 启动 `camera_aiq_test` 后 | `camera-aiq>` |

### 3.2 相机映射

| camera_id | sensor | ISP/NV12 | RAW10 | sensor subdev | IQ目录 |
|---:|---|---|---|---|---|
| 0 | `m00_b_imx586 4-001a` | `/dev/video22` | `/dev/video0` | `/dev/v4l-subdev4` | `/etc/iqfiles/cam0` |
| 1 | `m01_b_imx586 5-001a` | `/dev/video31` | `/dev/video11` | `/dev/v4l-subdev9` | `/etc/iqfiles/cam1` |

设备节点可能随内核或 media graph变化。重复测试时必须先查看 `CAMERA_INIT` 和
`capture-status all`，不能永久写死节点编号。

### 3.3 串口和网络

| 链路 | 参数/地址 | 用途 |
|---|---|---|
| 电脑调试串口 | `/dev/ttyUSB0`，1500000 | 登录板卡 Shell，不是 MCU控制串口 |
| MCU控制串口 | 板端 `/dev/ttyS9`，115200、8N1、无流控 | 参数命令、ACK/NACK、同步事件 |
| USB虚拟网口 | 板卡 `192.168.55.1`，电脑本次 `192.168.55.3/24` | ping、SSH、HTTP |
| HTTP | `http://192.168.55.1:8080/cam0`、`/cam1` | 双路 MJPEG |

### 3.4 IQ文件

两路程序分别加载：

```text
/etc/iqfiles/cam0/imx586_default_default.json
/etc/iqfiles/cam1/imx586_default_default.json
```

两份文件本次 SHA-256均为：

```text
d0f4a039190140e17995441f72dd162c0157e85ddea808281d3010cb508bb027
```

这证明独立路径和独立 RKAIQ context 已建立，但两份 IQ内容相同，不能证明广角镜头和
鱼眼镜头已经分别完成不同标定。

---

## 4. 测试前检查和启动方式

### 4.1 板卡基础检查

在板卡 Shell执行：

```bash
date -Ins
uname -a
df -h /root
free -m
v4l2-ctl --list-devices
for d in /dev/media*; do media-ctl -p -d "$d"; done
sha256sum /root/camera_uart/camera_aiq_test
sha256sum /etc/iqfiles/cam0/imx586_default_default.json
sha256sum /etc/iqfiles/cam1/imx586_default_default.json
```

### 4.2 避免两个进程争抢相机

```bash
pgrep -a camera_aiq_test
pgrep -a rkaiq_3A_server
```

同一时间只允许一个 `camera_aiq_test` 管理两路相机。不要在程序运行时再用板端
`v4l2-ctl` 抢占 `/dev/video22` 或 `/dev/video31`。

### 4.3 服务模式和交互模式的区别

正常产品运行使用：

```bash
systemctl start camera-uvc.service
systemctl status camera-uvc.service --no-pager
```

需要手工输入 `camera-aiq>` 命令时，应从调试串口进入板卡，再执行：

```bash
systemctl stop camera-uvc.service
cd /root/camera_uart
./camera_aiq_test
```

注意：`systemctl stop camera-uvc.service` 会结束整个进程，复合 USB和RNDIS可能暂时撤下，
所以不要通过同一条 RNDIS SSH会话执行这个操作。测试 UVC软停止时，应在程序中输入
`uvc-stop all`，不能用停止 systemd服务代替。

---

## 5. 阶段 0：无 MCU 软件自检

### 5.1 电脑端自检

在源码目录执行：

```bash
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart
make -f Makefile.camera_aiq \
  check-xvs-uart \
  check-control-uart-host \
  check-time-sync \
  check-stage6-host \
  check-stage7-host
```

本次输出：

```text
XVS_UART_MOCK_TEST_OK one_uart_mux=CAM,XVS_ACK,EVT
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14 pty=115200_8N1_ACK"
TIME_SYNC_SERVICE_TEST_OK state=HOLDOVER utc_valid=0
STAGE6_HOST_TEST_OK
STAGE7_TIME_PIPELINE_TEST_OK
```

### 5.2 板端程序自检

```bash
cd /root/camera_uart
./camera_aiq_test --control-uart-protocol-self-test
./camera_aiq_test --sync-protocol-self-test
./camera_aiq_test --sync-bind-self-test
./camera_aiq_test --photo-exif-self-test
```

本次四项退出码均为0，输出包括：

```text
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14"
XVS_PROTOCOL_SELF_TEST_OK
SYNC_BIND_SELF_TEST_OK source=SIM triggers=2 pairs=2 frame_delta_ns=200000
PHOTO_EXIF_SELF_TEST_OK
```

判定：**软件通过**。`utc_valid=0` 表示没有真实时间源，不能写成 UTC已锁定。

---

## 6. 阶段 1：双相机同时出图

### 6.1 检查目标

- 识别两颗不同地址的 IMX586；
- 两路 media graph完整；
- 两路 ISP同时产生4000x3000图像；
- 两路 RAW10同时能抓帧；
- 电脑端两个 UVC节点都能取得有效画面。

### 6.2 本次实测

| 测试项 | cam0 | cam1 | 判定 |
|---|---:|---:|---|
| UVC连续取帧 | 100帧，25.1秒 | 100帧，50.07秒 | 通过 |
| UVC文件大小 | 94,955,912字节 | 81,007,444字节 | 通过 |
| JPEG解析 | 100帧 | 99帧 | cam1有一个尾帧解析差异，主体通过 |
| 并发取流 | 4 fps请求，40个有效帧 | 2 fps请求，40个有效帧 | 通过 |
| 图像尺寸 | 4000x3000 | 4000x3000 | 通过 |
| 画面视角 | cam0独立视角 | cam1独立视角 | 通过 |

电脑端重复测试时先找 UVC节点：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --list-formats-ext
v4l2-ctl -d /dev/videoY --list-formats-ext
```

再并发保存，例如：

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=4 --stream-mmap=4 --stream-count=40 \
  --stream-to=cam0_40f.mjpeg

v4l2-ctl -d /dev/videoY \
  --set-fmt-video=width=4000,height=3000,pixelformat=MJPG \
  --set-parm=2 --stream-mmap=4 --stream-count=40 \
  --stream-to=cam1_40f.mjpeg
```

本次双路首尾帧联系图：

```text
test_results/20260805_v02_full_retest/host/concurrent_uvc_4hz_2hz_contact_sheet.jpg
SHA-256: 70159845c7049c9f19d6ad108ea7f30eee4d181f64e9722c2da4aebbdcd4cffe
```

阶段结论：**实机通过**。

---

## 7. 阶段 2：独立 ISP/IQ、曝光、增益和 ISO

### 7.1 测试命令

在 `camera-aiq>` 中执行：

```text
stream-start all
wait 5000
auto 0
auto 1
wait 3000
status all

exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
wait 3000
status all

exposure 0 10000
wait 2000
status all

gain 1 12000
wait 2000
status all

iso 0 150
iso 1 600
wait 3000
status all
```

### 7.2 实测回读

| 步骤 | cam0实测 | cam1实测 | 隔离判定 |
|---|---|---|---|
| 自动基线 | 曝光约30004 us | 曝光约30004 us | 基线正常 |
| 初次手动 | 请求5000 us/2000，回读5004 us/2000，ISO100 | 请求20000 us/8000，回读19996 us/8000，ISO400 | 通过 |
| 只改cam0曝光 | 回读9998 us/2000 | 保持19996 us/8000 | 通过 |
| 只改cam1增益 | 保持9998 us/2000 | 回读19996 us/12000 | 通过 |
| 独立ISO入口 | ISO150，对应增益3000 | ISO600，对应增益12000 | 通过 |

两路均出现 `manual_settings_verified=1`。曝光误差小于2%或100 us，增益误差小于2%或
50，且修改一侧时另一侧没有跟随变化。

### 7.3 ISO为什么有两个字段

当前 `status` 中：

- `aiq_iso=0`：RKAIQ原始接口没有给出有效 ISO；
- `iso`：程序按基础ISO 50与总增益换算出的有效控制值；
- `iso_estimated=1`：明确告诉测试人员该值是换算值。

因此可以判定“ISO/增益映射控制和照片元数据一致”通过，不能宣称“RKAIQ原生 ISO
回读”通过。

阶段结论：**实机通过**。限制是两份 IQ内容相同，且 ISO为按基础 ISO 50和总增益
换算的估算值，不是 RKAIQ原生 ISO回读。

---

## 8. 阶段 3及任务计划第四部分：帧率测试

### 8.1 必须区分两种帧率

1. `CAPTURE_STATUS fps_x1000` 和10秒 `frames` 增量：sensor/ISP底层真实输入速度。
2. 电脑 UVC `--set-parm=2/4`：从已有底层帧中按2/4 fps送给电脑的输出节流。

需求“左右相机分别设置帧率”必须验证第1种。只验证 UVC输出速度不够。

### 8.2 按要求测试四种组合

每组都在 `camera-aiq>` 中按以下结构执行：

```text
fps 0 CAM0_FPS
fps 1 CAM1_FPS
capture-status all
wait 10000
capture-status all
```

本次结果：

| 组合 | 10秒目标增量 cam0/cam1 | 10秒实测增量 cam0/cam1 | 实际状态 | 判定 |
|---|---:|---:|---|---|
| 4/4 | 约40/40 | 261/220 | 约26.1/22.0 fps | 不通过 |
| 4/2 | 约40/20 | 249/231 | 约24.9/23.1 fps | 不通过 |
| 2/4 | 约20/40 | 242/239 | 约24.2/23.9 fps | 不通过 |
| 2/2 | 约20/20 | 257/223 | 约25.7/22.3 fps | 不通过 |

日志同时出现：

```text
ERROR command=fps ... reason="target fps did not stabilize"
ERROR command=fps-xvs-thin ... code=-3 reason="IQ directory or memory error"
```

第二条错误文字不准确。`code=-3` 在这里实际是 sensor ioctl返回 I/O错误，不是 IQ目录
损坏。

### 8.3 根因

- 应用中的 `switch_xvs_camera_fps()` 会调用 `camera_backend_set_xvs_fps()`；
- 后端通过 `RKMODULE_SET_XVS_INPUT_THIN` 设置每颗 sensor的 XVS输入抽帧；
- `imx586.c` 只在 `xvs_slave_mode=true` 时接受该 ioctl；
- 当前 DTS为自由运行，两颗 sensor都不是 XVS从模式；
- 2 Hz配置被驱动拒绝，4 Hz只记录了请求目标，没有改变 sensor真实帧率。

### 8.4 正确修复和复测条件

接 MCU后应：

1. 刷回启用 XVS从模式的 DTS；
2. MCU持续输出共享4 Hz XVS，周期250000 us，低脉冲约10 us，空闲高；
3. 两颗 sensor都接收同一条4 Hz XVS；
4. 每颗 sensor独立使用 `xvs_input_thin=0/1`选择4 Hz或2 Hz；
5. 重新执行四种组合；
6. 10秒帧增量应接近40/40、40/20、20/40、20/20；
7. 同时检查 `sequence_drops`、MIPI错误和示波器波形。

阶段结论：**不通过**。当前程序接口已存在，但没有形成真实2/4 Hz sensor输出。

---

## 9. 阶段 4：UART 115200、8N1统一控制

任务要求 UART命令均带 `camera_id`，由统一应用层程序调用 V4L2/RKAIQ和输出服务，
不从应用层直接裸写 sensor寄存器。

本次已验证：

- 115200、8数据位、1停止位、无校验、无流控的软件配置；
- 14个控制协议用例；
- ACK/NACK、CRC和错误解析；
- 相机曝光、增益、ISO、帧率、保存、同步、UVC、网口和状态命令的分发路径；
- 电脑伪终端测试和板端程序自检均通过。

未验证：

- RK3576 `/dev/ttyS9` 与真实 MCU之间的双向实线；
- MCU发命令后相机真实执行；
- 线缆断开、CRC错误、超时和重连恢复；
- 1.8 V与 MCU电平转换的现场信号质量。

阶段结论：**软件通过**；真实 MCU UART实线为**待硬件验收**。

---

## 10. 阶段 5：Trigger、frame_id和硬件同步

### 10.1 本次无 MCU模拟测试

在 `camera-aiq>` 中执行：

```text
time-sync-reset
sync-bind-reset 1
sync-bind-log /root/camera_uart/v02_full_retest_20260805/photo_bind.csv
sync-sim-start 4 12
wait 4000
sync-sim-status
sync-status
sync-bind-status
sync-bind-last
time-sync-status
```

### 10.2 实测结果

- 模拟产生12个 Trigger；
- 首个 Trigger作为预备帧忽略；
- Trigger 2～12形成11组左右帧；
- `complete_pairs=11`；
- `trigger_id_gaps=0`；
- `duplicate_triggers=0`；
- `pending=0`，`pending_overflows=0`；
- 最后一组 cam0 `frame_id=1322`，cam1 `frame_id=1314`，共同 `trigger_id=12`。

但当前是自由运行画面按“最近 V4L2单调时间戳”配对，日志明确为：

```text
method=nearest_v4l2_monotonic_timestamp diagnostic_only=1
```

左右帧时间差在约0.875 ms与67.5 ms之间交替，最后一组约67.499 ms。这不是硬件同步
结果，不能用于宣称两颗 IMX586曝光同时开始。

### 10.3 接 MCU后的正式验收

示波器至少同时观察：共享 XVS、cam0帧/曝光指示、cam1帧/曝光指示。每种4/2 Hz组合
至少记录：XVS周期、脉宽、两路相位差、Trigger到帧事件延迟、丢触发数和帧计数。

阶段结论：Trigger/frame_id绑定**软件通过**；真实 XVS硬件同步为**待硬件验收**。

---

## 11. 阶段 6：照片、EXIF、时间和逐帧信息

### 11.1 测试命令

```text
exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
photo-offset 0 0
photo-offset 1 0
photo-start 0 /root/camera_uart/v02_full_retest_20260805/cam0_photo
photo-start 1 /root/camera_uart/v02_full_retest_20260805/cam1_photo
sync-sim-start 4 12
wait 4000
photo-stop 0
photo-stop 1
photo-status all
```

### 11.2 保存结果

| 指标 | cam0 | cam1 |
|---|---:|---:|
| JPEG数量 | 11 | 11 |
| queue_drops | 0 | 0 |
| invalid_metadata | 0 | 0 |
| encode_errors | 0 | 0 |
| exif_errors | 0 | 0 |
| write_errors | 0 | 0 |
| 最后一张大小 | 988,448字节 | 1,152,344字节 |

Trigger 12的文件名、JPEG EXIF/UserComment和 CSV一致：

| 字段 | cam0 | cam1 |
|---|---:|---:|
| camera_id | 0 | 1 |
| frame_id | 1322 | 1314 |
| trigger_id | 12 | 12 |
| exposure_us | 5004 | 19996 |
| gain_x1000 | 2000 | 8000 |
| ISO | 100 | 400 |

限制：

- `iso_estimated=1`，ISO来自增益映射；
- `utc_valid=0`，未接 PPS/GNSS；
- 板端系统时间当时为 `2026:06:05 16:20:09`，不是有效卫星 UTC；
- Trigger来源为 `SIM`；
- 当前只能证明固定手动参数与 frame_id绑定正确；
- 参数在相邻帧快速变化时，需要进一步读取或锁存逐帧 sensor寄存器快照。

阶段结论：JPEG、EXIF、CSV和逐帧参数绑定**软件通过**；真实 UTC/PPS和曝光相位为
**待硬件验收**。

---

## 12. 阶段 7：RAW10、eMMC、UVC、HTTP和USB网口

### 12.1 RAW10双路实抓

板端节点：cam0 `/dev/video0`，cam1 `/dev/video11`。两路均为4000x3000 `RG10`。

实测每帧15,360,000字节，符合10-bit packed并按5120字节行对齐的大小。

```text
cam0 SHA-256: 6b5f815b4b3ca711ee47af0f00bc149f58f0faf49c22274c7b042c9d10a72420
cam1 SHA-256: ca6ff4d46b32e0aee14be03b6f332a34659cee4a66d9c1a72f73ce7e46fbf66c
```

判定：双 RAW10实际成帧**通过**。

### 12.2 eMMC保存

在 `camera-aiq>` 中执行：

```text
save-start 0 /root/camera_uart/v02_full_retest_20260805/cam0_nv12
save-start 1 /root/camera_uart/v02_full_retest_20260805/cam1_nv12
wait 10000
save-stop 0
save-stop 1
capture-status all
```

本次每张 NV12为18,000,000字节：

| 指标 | cam0 | cam1 |
|---|---:|---:|
| 10秒保存 | 44帧 | 45帧 |
| 写入量 | 792 MB | 810 MB |
| save_failures | 0 | 0 |
| save_queue_drops | 226 | 454 |

产生队列丢帧的主要原因是底层仍以约22～30 fps输入，而不是目标4/2 Hz；同时 `/root`
分区测试后约93%已用，不适合继续大量保存全分辨率 NV12。

判定：eMMC双路能保存，**高输入率无丢帧不通过**。等 XVS 4/2 Hz修复后必须复测。

### 12.3 双 HTTP网口输出

板端程序中：

```text
net-start 0
net-start 1
wait 5000
net-status all
```

电脑端：

```bash
curl -I http://192.168.55.1:8080/
timeout 20 curl -sS http://192.168.55.1:8080/cam0 > cam0_http.mjpeg
timeout 20 curl -sS http://192.168.55.1:8080/cam1 > cam1_http.mjpeg
ffprobe -v error -count_frames -show_entries stream=width,height,nb_read_frames cam0_http.mjpeg
ffprobe -v error -count_frames -show_entries stream=width,height,nb_read_frames cam1_http.mjpeg
```

本次双路并发约22秒：

| 指标 | cam0 | cam1 |
|---|---:|---:|
| HTTP状态 | 200 | 200 |
| 接收字节 | 150,787,284 | 136,556,925 |
| 有效JPEG帧 | 218 | 219 |
| 尺寸 | 4000x3000 | 4000x3000 |

判定：双 HTTP真实图像输出**通过**。

### 12.4 RNDIS、ping和SSH

电脑端：

```bash
ip addr
ping -c 10 192.168.55.1
ssh root@192.168.55.1
```

本次 ping 10发10收、0%丢包，平均约0.305 ms；SSH真实登录并输出
`SSH_RNDIS_OK`。判定：RNDIS和SSH**通过**。

### 12.5 双 UVC与网口并发

本次双 UVC、双 HTTP和RNDIS同时运行时：

- 两个 UVC节点都取得有效4000x3000 MJPEG；
- `/cam0`、`/cam1` 同时持续有图；
- ping 0%丢包；
- SSH可正常登录。

但电脑 `lsusb -t` 实测链路为 `480M`，即 USB 2.0 high-speed，不是 USB 3.0
SuperSpeed。当前只可判 USB 2.0复合功能并发通过，USB 3.0验收**不通过**。

### 12.6 UVC软停止不能影响网口

程序内执行：

```text
uvc-stop all
wait 2000
uvc-status all
net-status all
capture-status all
```

本次程序返回：

```text
OK command=uvc-stop target=all usb_gadget=kept rndis=kept
```

停止 UVC送帧期间实测（run3）：

- UDC始终为 `configured`；
- 板卡 `usb0`保持 UP，地址仍为 `192.168.55.1/24`；
- 电脑 USB设备保持 `Bus 001 Device 012: ID 2207:0017`，没有重新枚举；
- ping 5发5收、0%丢包，平均5.288 ms；
- 停止期间重新登录 SSH成功，读到 `usb0 UP 192.168.55.1/24`；
- 双 HTTP均返回200，6秒内分别收到约52.18 MB和47.76 MB；
- 两个 UVC节点在7秒测试窗口内均取得0帧并超时，证明 UVC确实暂停；
- 两路底层采集仍为 `running=1`。

随后执行：

```text
uvc-start all
wait 3000
uvc-status all
```

cam0和cam1均重新成功取得8帧/4 fps和4帧/2 fps的4000x3000 MJPEG，USB设备仍为
同一 `Device 012`；双 HTTP均返回200，ping 5/5、0%丢包，SSH端口正常。

重要使用规则：

- 暂停相机 UVC输出：使用程序命令 `uvc-stop all`；
- 恢复：使用 `uvc-start all`；
- 不要用 `systemctl stop camera-uvc.service` 当作 UVC软停止，因为它会结束整个进程并
  撤下复合USB，RNDIS也会断开。

阶段结论：双UVC、双HTTP、RNDIS、SSH和软停止网络连续性**实机通过**；USB 3.0和
高输入率 eMMC零丢帧**不通过**。本轮另外完成双 UVC、双 HTTP、RNDIS/ping 并发
30 分钟长稳：
HTTP持续返回200，ping 0%丢包，UVC两路持续有帧；但UVC工具累计出现缓冲丢弃，不能
写成“零丢帧通过”。

---

## 13. 资源、温度、长稳和功耗

本次采集199组、约200秒资源样本：

| 指标 | 最低 | 平均 | 最高 |
|---|---:|---:|---:|
| 温度 | 44.384°C | 52.182°C | 56.384°C |
| 1分钟load | 0.00 | 0.21 | 1.32 |
| MemAvailable | 3,199,476 kB | 3,385,192 kB | 3,790,536 kB |

内存下降与约1.6 GB NV12文件页缓存相符，本组短样本不能证明存在内存泄漏；后续
已用独立的30分钟并发测试补足持续输出观察。

### 13.1 本轮30分钟并发长稳补测（run3）

测试同时保持双路 UVC、双路 HTTP、RNDIS/ping 和相机服务运行，未把图像保存到
eMMC，板端每分钟记录服务、UDC、负载、内存和温度。证据文件位于：
`test_results/20260805_v02_full_retest/run3/stress30_*.log` 和
`run3/retest_run3_stress_board.log`。

| 项目 | 结果 | 判定说明 |
|---|---:|---|
| 测试时长 | 30 min（约 1800 s） | 通过；curl 和 ping 日志均覆盖约 30 分钟 |
| cam0 UVC | 1799个速率样本，平均4.134 fps，范围4.03～5.90 fps | 通过有流；202行 dropped-buffer提示，累计460个buffer |
| cam1 UVC | 1785个速率样本，平均2.120 fps，范围2.03～3.07 fps | 通过有流；180行 dropped-buffer提示，累计408个buffer |
| cam0 HTTP | HTTP 200，16,088,441,756 bytes，约8.94 MB/s | 通过；`curl (28)`为1800秒主动结束 |
| cam1 HTTP | HTTP 200，14,902,341,180 bytes，约8.28 MB/s | 通过；`curl (28)`为1800秒主动结束 |
| RNDIS/ping | 1783/1783，0%丢包 | 通过，日志无丢包 |
| 服务/UDC | 31/31个样本为`active`/`configured` | 监控期间未退出或变为detached |
| USB链路 | 31/31个样本为`high-speed` | 功能稳定；物理速率仍为USB2.0 |
| 温度 | 67.461～75.769°C，平均约74.340°C | 通过基础热稳定观察；无热保护触发 |
| MemAvailable | 最小2,948,588 kB，平均2,986,709 kB，最大3,129,748 kB | 未见持续下降 |

该补测证明当前自由运行版本在并发输出场景下可以连续工作，但不能替代真实
2/4 Hz sensor 验收、功耗仪测量，也不能宣称 UVC 零丢帧。仍需关注 USB 主机重协商
时的 DWC3 endpoint 告警和高输入帧率下的队列丢弃。

### 13.2 仍未完成

- 待机、单路、双路、UVC、网口、eMMC、同步的功耗仪测量；
- CPU/NPU/ISP/DDR 完整占用记录；
- YOLO 集成和附加功耗。

判定：30 分钟并发持续输出和网络连续性**实机通过**，但零丢帧要求**不通过**；温度
和基础资源正常；功耗和 YOLO为**待硬件验收**。

---

## 14. 内核和稳定性风险

本次日志发现以下告警：

1. RAW启动时出现过 `MIPI_CSI2 ERR2:0xf0000`；
2. 反复启停时 cam0出现过 `csi size err`；
3. 两路均提示 `vblank need >= 1000us ... cur 696 us`；
4. 程序退出时 RKAIQ有 pool item仍在使用；
5. 电脑停止 UVC流时出现过 `Unable to queue buffer: No such device (19)`，随后应用正常
   执行 `Stopping video stream`，本次能够再次打开；
6. DWC3日志中出现少量 endpoint request未入队提示。

这些告警没有阻止本次成帧和 UVC恢复。30分钟并发期间服务未退出，但仍建议在产品
验收中统计 MIPI错误增量、启停次数、端点恢复次数和资源释放状态，并继续做60分钟
及更长时间的压力测试。

---

## 15. 接 MCU后的必做复测

### 15.1 接线前

- 确认 RK3576 UART9所在 VCCIO为1.8 V；
- MCU为3.3 V时必须使用电平转换；
- UART TX/RX交叉连接并共地；
- MCU XVS连接到 `FSYNC_CAM`；
- 用示波器先确认 XVS空闲高、低脉冲约10 us、周期250000 us。

### 15.2 刷回 XVS从模式后先验证是否有帧

1. MCU先持续输出4 Hz XVS；
2. 再启动相机程序；
3. 执行 `stream-start all`；
4. 连续两次 `capture-status all`，确认两路约每秒增长4帧；
5. 如果 `frames=0`，先查 XVS电平、极性、DTS和 sensor寄存器，不要先查 UVC。

### 15.3 四种帧率组合正式验收

| 组合 | 10秒期望 cam0 | 10秒期望 cam1 |
|---|---:|---:|
| 4/4 | 约40帧 | 约40帧 |
| 4/2 | 约40帧 | 约20帧 |
| 2/4 | 约20帧 | 约40帧 |
| 2/2 | 约20帧 | 约20帧 |

每组执行：

```text
fps 0 <2或4>
fps 1 <2或4>
capture-status all
wait 10000
capture-status all
```

通过条件：帧增量符合目标容差、两路可独立切换、另一侧不受影响、无新增MIPI错误、
`sequence_drops=0`或满足项目允许值。

### 15.4 同步与 UTC

1. 示波器同时观察共享XVS和两路帧/曝光指示；
2. MCU上报PPS事件和GPRMC/GNRMC；
3. `time-sync-status` 必须进入LOCKED且 `utc_valid=1`；
4. 相同 Trigger必须绑定左右各一帧，不能重复或跳号；
5. 照片文件名、CSV、EXIF和UserComment中的 camera_id、frame_id、trigger_id、UTC、
   曝光、增益必须一致；
6. 标定 sensor响应延迟后，再验收 exposure_start和 exposure_center。

---

## 16. 证据索引

本轮电脑端证据目录：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/
  test_results/20260805_v02_full_retest/host
```

主要文件：

| 文件 | 内容 |
|---|---|
| `camera_aiq_batch.log` | 阶段2～7完整程序命令与输出 |
| `stage0_host_selftests.log` | 电脑端软件自检 |
| `stage0_board_selftests.log` | 板端程序自检 |
| `stage1_board_inventory.log` | sensor、media、V4L2和USB清单 |
| `stage1_cam0_100f.mjpeg` | cam0 UVC 100帧 |
| `stage1_cam1_100f.mjpeg` | cam1 UVC 100帧 |
| `stage1_contact_sheet.jpg` | 双路画面对比 |
| `concurrent_cam0_40f.mjpeg` | cam0并发4 fps输出证据 |
| `concurrent_cam1_40f.mjpeg` | cam1并发2 fps输出证据 |
| `concurrent_uvc_4hz_2hz_contact_sheet.jpg` | 双路并发首尾帧联系图 |
| `concurrent_http_cam0.mjpeg` | cam0 HTTP并发流 |
| `concurrent_http_cam1.mjpeg` | cam1 HTTP并发流 |
| `board_evidence/photo_bind.csv` | Trigger与左右 frame_id绑定 |
| `board_evidence/resource_samples.log` | 温度、load和内存样本 |
| `board_evidence/uvc_soft_stop_console.log` | UVC软停止和恢复全过程 |
| `soft_stopped_http_cam0.jpg` | UVC停止期间cam0 HTTP画面 |
| `soft_stopped_http_cam1.jpg` | UVC停止期间cam1 HTTP画面 |
| `soft_recovered_uvc_cam0.mjpeg` | UVC恢复后的cam0流 |
| `soft_recovered_uvc_cam1.mjpeg` | UVC恢复后的cam1流 |
| `board_evidence/dmesg_before_uvc_soft_stop.log` | 软停止前内核日志 |
| `board_evidence/dmesg_after_uvc_soft_stop.log` | 软停止后内核日志 |

### 16.1 历史 run2 30分钟并发补测证据

以下文件在 `test_results/20260805_v02_full_retest/run2/`：

| 文件 | 内容 |
|---|---|
| `stress30_uvc_cam0.log` / `stress30_uvc_cam1.log` | 两路 UVC 持续取帧、实际输出速率和 dropped buffers |
| `stress30_http_cam0.log` / `stress30_http_cam1.log` | HTTP 状态码、30分钟累计字节和平均速率 |
| `stress30_ping.log` | RNDIS 30分钟 ping 原始输出及丢包统计 |
| `post_stress_cam0.mjpeg` / `post_stress_cam1.mjpeg` | 历史长稳结束后的 HTTP 冒烟流，均解析为4000x3000 |

### 16.2 run3 生命周期和最终冒烟证据

| 文件 | 内容 |
|---|---|
| `lifecycle_interactive_start.log` | 交互程序启动、双路 HTTP/UVC 启动 |
| `lifecycle_stopped_board_status.log` | `uvc-stop all` 后 UVC=0、HTTP=1、采集仍运行 |
| `lifecycle_stopped_uvc_cam0.log` / `lifecycle_stopped_uvc_cam1.log` | 停止期间两个 UVC 节点0帧超时 |
| `lifecycle_stopped_ping.log` / `lifecycle_stopped_ssh_session.log` | 停止期间网络和真实 SSH 登录 |
| `lifecycle_stopped_http_cam0.log` / `lifecycle_stopped_http_cam1.log` | 停止期间双 HTTP 200和非零字节 |
| `lifecycle_restart_board.log` | `uvc-start all` 后恢复状态 |
| `lifecycle_restarted_cam0_8f.mjpeg` / `lifecycle_restarted_cam1_4f.mjpeg` | UVC恢复后的双路帧 |
| `lifecycle_daemon_restore_board.log` | 退出交互程序并恢复常驻服务 |
| `final_smoke_*` | 最终双 UVC、双 HTTP、ping、SSH端口和 USB 冒烟 |

run3关键日志哈希：

```text
retest_run3_stress_board.log  523ba300939e257fc76faa7f21fe95f9524b95825c76df6050062a48f34ec636
stress30_http_cam0.log        1dd9f96f0327e1fb2d48c5046c1863102663b47106f2616268cf5e81d042694d
stress30_http_cam1.log        13376b51cd3b6998bc6e97f0c1cfbcc8dbdef50be6790cf22f9c1afd73841e95
stress30_ping.log             8a6a74a6d978bb552e150560304cd686fa277cd4df351a46b898335e351810c3
stress30_uvc_cam0.log         197597fd49e626e40b3d512f4248ad483ce46c498b733e6251f87fa45bb2350d
stress30_uvc_cam1.log         393d12e1b7987fb18e4ccaff1cd40af705d9cfdba28e4b548326fa84f3020634
```

历史 run2 证据中 HTTP 日志的 `curl (28)` 是测试命令的 1800 秒限时到期，不是 HTTP
返回失败；两路状态码均为 200。UVC日志的 `dropped buffers: 2` 是主机取流过程中的
缓冲丢弃，故历史长稳结论写为“有流但非零丢帧”。run3 的准确数据见 13.1 和 16.2。

关键哈希：

```text
rk3576_v02_run2_evidence.tgz:
e82a298dad8f3cbce30c22fd24ddbc63cac49da691a65f037fbc237937cffab9

post_stress_cam0.jpg:
55899c0acd6602b11606ee225e2c6d908f80777a92f2c1762f7de4e5a12980b5

post_stress_cam1.jpg:
58c6e290b9bdf40f76c77433dd5b51fd5242cd64c74fcaa97b0d49a3bd520326
```

小证据包：

```text
test_results/20260805_v02_full_retest/host/evidence_small.tar.gz
SHA-256: d9f36e48050f6c85411dae4318a31cb8fc2a0248447c4d73b4866a9d62a25fae
```

软停止关键证据哈希：

```text
uvc_soft_stop_console.log:
de6f639746bf47d238286efed67682e71d84f44282ec1fe919e973950e4ab6f9

dmesg_before_uvc_soft_stop.log:
b0787685464f7ec41402e9134039014def370089d4fb579f07c71ec0dd953ff7

dmesg_after_uvc_soft_stop.log:
ac2d9b22c07f7b42242c66a74a34b7416a20a285a95dddc63b57cebef32e0809
```

---

## 17. 最终签署意见

当前版本已经具备双 IMX586同时采集、两路参数独立控制、双 UVC、双 HTTP、RNDIS/SSH、
RAW10、JPEG/EXIF、frame_id绑定和 eMMC保存的主体软件能力。

本次不能签署“全部完成”的原因不是双摄或输出链路未实现，而是以下正式验收条件尚未
满足：

1. 当前没有 XVS从模式和 MCU外部4 Hz时基，sensor底层四种2/4 Hz组合全部不通过；
2. 未接 MCU，UART实线和真实硬件同步没有验收；
3. 未接 PPS/GNSS，有效 UTC和真实曝光相位没有验收；
4. 当前 USB只有480 Mbps，不是 USB 3.0；
5. eMMC在约24 fps输入时发生保存队列丢帧；
6. 30分钟并发长稳已完成，但存在 UVC 缓冲丢弃；60分钟以上长稳、功耗仪和 YOLO仍未验收；
7. MIPI、vblank和UVC退出路径存在需继续观察的内核告警。

建议下一版首先接入 MCU并刷回 XVS从模式，优先关闭阶段3的四种帧率失败项；然后按
第15节完成 UART、XVS、PPS/UTC和示波器正式验收，最后再做 USB 3.0直连、长稳和功耗。

---

## 18. XVS顺序验证检查点（2026-08-05）

本轮按“先确认XVS条件、再备份、最后刷写”的顺序执行，结果停在安全刷写条件之前。

### 18.1 已完成

1. 板卡RNDIS网络、SSH、`camera-uvc.service`和复合USB仍正常；服务启动参数为
   `camera_aiq_test --all-daemon`。
2. 当前运行设备树没有 `sony,xvs-slave-mode` 和 `sony,xvs-input-thin`，确认板卡
   仍运行自由运行版本。
3. 反编译候选镜像
   `backups/20260805_before_no_mcu_freerun/boot.img` 的最终FIT FDT，确认：
   - module-index 0和1两颗IMX586均有 `sony,xvs-slave-mode`；
   - 两路均为 `sony,xvs-input-thin = <0>`，上电默认都是4 Hz输入模式。
4. 已完整备份板卡实际boot分区：

```text
backups/20260805_before_xvs_ordered_test/boot_partition_64MiB.img
size=67108864
sha256=5c56270d2fc63f07939f9a8fa84532f8d9b1b16ea073c202b26cb1d8dddfff54
```

该分区前46454272字节与当前SDK `kernel-6.1/boot.img` 逐字节一致，恢复基线已确认。

### 18.2 MCU/XVS前置探测

将 `/dev/ttyS9` 配置为115200、8N1、无流控后，发送带正确CRC的：

```text
$XVS,1,PING*2D89\r\n
```

连续等待3秒，接收文件为0字节，没有收到 `PONG`、ACK、NACK或任何异步事件。
同时当前服务未带 `--uart /dev/ttyS9` 和 `--xvs-autostart-hz 4` 参数。因此本轮没有
证据证明MCU已接入，也没有证据证明 `FSYNC_CAM` 上存在4 Hz、空闲高、低脉冲约10 us
的外部XVS。

### 18.3 为什么未刷写

XVS候选镜像本身已经核验正确，但从模式下IMX586必须等待外部边沿。没有先确认外部
4 Hz脉冲就刷写，会导致两路sensor都没有帧，后续 `4/4`、`4/2`、`2/4`、`2/2`、
双UVC、HTTP和eMMC测试均无法得到有效结论。为保留当前可工作的双路输出，本轮没有
刷写候选镜像。

### 18.4 继续测试的唯一前置条件

接入MCU或等效硬件信号源后，必须先完成：

1. UART `PING` 返回合法 `PONG`，`IDLE` 返回ACK；
2. 示波器在 `FSYNC_CAM` 上测得空闲高、4 Hz（周期约250 ms）、低脉冲约10 us；
3. 确认XVS电平满足板卡和IMX586的1.8 V链路要求。

三项满足后，从本检查点继续：启动共享4 Hz、刷XVS候选boot、重启，先验收两路4 Hz，
再依次执行 `4/4`、`4/2`、`2/4`、`2/2`，最后复测双UVC、HTTP/RNDIS和eMMC。
