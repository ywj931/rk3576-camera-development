# RK3576 双 IMX586 独立 2/4 Hz 手动测试文档

日期：2026-08-04

## 1. 测试目的

本测试确认以下要求：

1. cam0 和 cam1 都可以独立选择 2 Hz 或 4 Hz；
2. 支持 `4/4`、`2/4`、`4/2`、`2/2` 四种组合；
3. 修改一路帧率时，另一路继续采集，不停止计帧；
4. 修改一路帧率时，不改变另一路曝光和增益；
5. 被切换相机原来处于手动曝光时，重启后恢复原曝光和增益；
6. 两路 UVC 都能按照各自设置的 2 Hz 或 4 Hz 输出到电脑。

这里的 `fps 0 2` 和 `fps 1 4` 分别控制 cam0 和 cam1，不是把 cam0 固定为
4 Hz、cam1 固定为 2 Hz。

## 2. 工作原理

两颗 IMX586 共用一根 XVS 信号线，因此外部 MCU 或信号发生器必须始终提供：

```text
4 Hz、低有效、低脉宽约 10 us、空闲高电平
```

每颗 IMX586 再独立决定如何接收这根共享 XVS：

| 相机目标帧率 | xvs_input_thin | 含义 |
| --- | --- | --- |
| 4 Hz | 0 | 接收每个 XVS 脉冲 |
| 2 Hz | 1 | 每两个 XVS 脉冲接收一次 |

所以共享 XVS 不允许为了某一路切换成 2 Hz。执行 `fps CAMERA_ID 2|4` 时，只修改
指定 IMX586 的输入分频。

IMX586 的分频寄存器只能在软件待机时修改。被切换的一路会短暂停止并重启，另一
路不应停止。这里的“互不干扰”是指未切换路持续计帧且参数不变，不是目标路无缝
切换。

## 3. 必需条件

### 3.1 硬件

- RK3576 板卡和两颗 IMX586 均已连接；
- cam0、cam1 的 XVS 输入连接到同一根共享 XVS；
- MCU 已连接，或者用信号发生器替代 MCU；
- MCU UART 使用 `/dev/ttyS9`、115200、8N1；
- MCU 能持续输出 4 Hz、低有效约 10 us 的 XVS；
- 做 UVC 测试时，RK3576 的 Type-C Device 口通过 USB 3.0 数据线连接电脑。

### 3.2 软件

必须同时使用新内核和新应用，不能只替换其中一个：

```text
板卡 boot 分区：包含 IMX586 XVS 分频 ioctl 的新 boot.img
/root/camera_uart/camera_aiq_test：支持两路独立 2/4 Hz 的新应用
```

主机SDK中的对应文件是：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/kernel-6.1/boot.img
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/camera_aiq_test
```

若板卡还没有刷入新 `boot.img`，不要开始实测。仅更新应用会出现
`fps-xvs-query` 错误。

### 3.3 没有 MCU 时能测试什么

没有 MCU 或信号发生器时，XVS 从模式下两颗 IMX586会等待脉冲，通常表现为
`running=1` 但 `frames=0`。这时只能检查程序、驱动接口和静态配置，不能验收
真实 2/4 Hz、双路连续性或 UVC 实流。

`sync-sim-start` 只模拟应用层 trigger 事件，不会在物理引脚产生 XVS，不能代替
本测试的外部 4 Hz 信号。

## 4. 测试连接关系

```text
MCU 4 Hz XVS ----+----> cam0 IMX586 XVS input
                 |
                 +----> cam1 IMX586 XVS input

MCU UART <------------> RK3576 /dev/ttyS9 (115200, 8N1)
RK3576 Type-C Device --> 电脑 USB 3.0 口（UVC测试时使用）
```

## 5. 板卡端启动手动测试

下面命令全部在RK3576板卡终端执行。

### 5.1 检查并停止冲突程序

```sh
systemctl stop camera-uvc.service
systemctl stop camera-http.service 2>/dev/null || true
pkill -f rkaiq_3A_server 2>/dev/null || true
pkill -f v4l2-ctl 2>/dev/null || true
pkill -f v4l2_frame_tap 2>/dev/null || true
fuser /dev/ttyS9 /dev/video22 /dev/video31
```

最后一条正常情况下没有输出。如果显示进程号，先确认并停止占用进程。不要让
`rkaiq_3A_server`、`v4l2-ctl` 或第二个 `camera_aiq_test` 同时占用相机。

### 5.2 启动交互程序

```sh
cd /root/camera_uart
chmod +x camera_aiq_test
./camera_aiq_test --uart /dev/ttyS9 --sync-timer-hz 1000000
```

程序启动时会向 MCU 执行 `PING` 和 `IDLE`。正常应看到类似：

```text
XVS_UART_READY device="/dev/ttyS9" baud=115200 format=8N1
UART_MUX_READY device="/dev/ttyS9" baud=115200 format=8N1
CAMERA_BACKEND_READY cameras=2 uvc=cam0,cam1:4000x3000@2-or-4fps/MJPEG
camera-aiq>
```

如果这里直接报 UART 超时或 CRC 错误，先修复 MCU 通信，不继续测试相机帧率。

### 5.3 启动两路采集和共享 4 Hz XVS

在 `camera-aiq>` 提示符后逐条输入：

```text
stream-start all
sync-start 4 10
wait 5000
sync-controller-status
status all
capture-status all
```

必须满足：

- `XVS_CONTROLLER_STATUS connected=1 valid=1`；
- `frequency_millihz=4000`、`low_pulse_us=10`；
- 两路 `STATUS online=1 started=1 query_valid=1`；
- 两路 `xvs_config_valid=1`；
- 两路 `CAPTURE_STATUS running=1`；
- 两路 `frames` 在两次查询之间持续增加；
- `last_errno=0`。

初始默认应为两路 4 Hz，因此两路应回读：

```text
xvs_input_thin=0
fps_target_x1000=4000
fps_x1000约为4000
```

## 6. 先设置不同曝光和增益

先给两路设置明显不同的参数，用于确认切换帧率时不会串路：

```text
exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
wait 3000
status all
```

记录两路的以下字段：

```text
mode
exposure_us
gain_x1000
requested_exposure_us
requested_gain_x1000
manual_settings_verified
```

通过条件：

- cam0 回读曝光约 5000 us、模拟增益约 2000；
- cam1 回读曝光约 20000 us、模拟增益约 8000；
- 两路 `mode=MANUAL`；
- 两路 `manual_settings_verified=1`。

`iso` 是 RKAIQ 根据整条增益链计算的回读结果。本步骤独立控制的判定字段以
`exposure_us`、`gain_x1000` 和 `manual_settings_verified` 为准。

## 7. 依次验证四种帧率组合

每条 `fps` 命令内部最多等待约 15 秒确认实测频率稳定。不要连续快速输入下一条。

### 7.1 组合一：cam0=4 Hz，cam1=4 Hz

```text
fps 0 4
fps 1 4
wait 3000
status all
capture-status all
```

预期：

| camera_id | xvs_input_thin | fps_target_x1000 | fps_x1000允许范围 |
| --- | --- | --- | --- |
| 0 | 0 | 4000 | 3800～4200 |
| 1 | 0 | 4000 | 3800～4200 |

### 7.2 组合二：cam0=2 Hz，cam1=4 Hz

```text
fps 0 2
wait 3000
status all
capture-status all
```

`fps 0 2` 的成功输出必须包含：

```text
requested_fps_x1000=2000
fps_stable=1
other_camera_id=1
other_camera_was_running=1
other_camera_continued=1
manual_parameters_restored=1
```

状态预期：

| camera_id | xvs_input_thin | fps_target_x1000 | fps_x1000允许范围 |
| --- | --- | --- | --- |
| 0 | 1 | 2000 | 1900～2100 |
| 1 | 0 | 4000 | 3800～4200 |

同时确认 cam1 的曝光仍约为 20000 us、增益仍约为 8000。

### 7.3 组合三：cam0=4 Hz，cam1=2 Hz

```text
fps 0 4
fps 1 2
wait 3000
status all
capture-status all
```

两条成功输出都必须包含 `fps_stable=1` 和 `other_camera_continued=1`。

状态预期：

| camera_id | xvs_input_thin | fps_target_x1000 | fps_x1000允许范围 |
| --- | --- | --- | --- |
| 0 | 0 | 4000 | 3800～4200 |
| 1 | 1 | 2000 | 1900～2100 |

同时确认 cam0 的曝光仍约为 5000 us、增益仍约为 2000。

### 7.4 组合四：cam0=2 Hz，cam1=2 Hz

```text
fps 0 2
wait 3000
status all
capture-status all
```

状态预期：

| camera_id | xvs_input_thin | fps_target_x1000 | fps_x1000允许范围 |
| --- | --- | --- | --- |
| 0 | 1 | 2000 | 1900～2100 |
| 1 | 1 | 2000 | 1900～2100 |

共享 MCU XVS 此时仍必须是 4 Hz：

```text
sync-controller-status
```

预期仍为 `frequency_millihz=4000`，不能变成 2000。

## 8. 独立性判定方法

每次只切换一路时，检查该条 `fps` 命令输出：

```text
fps_stable=1
other_camera_continued=1
manual_parameters_restored=1
```

然后比较切换前后的 `status all` 和 `capture-status all`：

- 未切换路 `frames` 必须增加；
- 未切换路 `running=1`；
- 未切换路的 `exposure_us` 和 `gain_x1000` 不变；
- 未切换路的 `xvs_input_thin` 不变；
- 被切换路恢复出流后 `manual_settings_verified=1`；
- 两路 `sequence_drops` 不持续增加；
- 两路 `last_errno=0`。

若另一相机本来没有启动，程序会打印 `other_camera_was_running=0`，这种结果不能
用于证明双路互不干扰。必须先执行 `stream-start all`。

## 9. 20轮反复切换测试

把下面四条作为一轮，手动重复20轮：

```text
fps 0 2
fps 1 4
fps 0 4
fps 1 2
```

完成后执行：

```text
status all
capture-status all
sync-controller-status
```

通过条件：

- 80条 `fps` 命令全部返回 `OK`；
- 每条都有 `fps_stable=1`；
- 每条都有 `other_camera_continued=1`；
- 两路最终仍有画面且帧计数增加；
- 两路曝光和增益仍为第6节设置的值；
- `last_errno=0`，没有持续增长的 `sequence_drops`。

## 10. 双 UVC 手动测试

### 10.1 板卡端启动双 UVC

仍在同一个 `camera_aiq_test` 控制台中输入：

```text
uvc-start all
uvc-status all
```

正常应看到两路：

```text
enabled=1
source_camera_id分别为0和1
configured=4000x3000@当前设置fps/MJPEG
encode_errors=0
last_error=0
```

电脑还没有打开视频时，`host_streaming=0`、`skipped_no_host` 增长是正常的。

### 10.2 电脑端找到两路 UVC 节点

下面命令在电脑Ubuntu终端执行：

```sh
v4l2-ctl --list-devices
```

找到RK3576对应的两个视频节点，分别记为：

```text
CAM0_UVC=/dev/videoX
CAM1_UVC=/dev/videoY
```

不要直接假定它们一定是 `/dev/video0` 和 `/dev/video2`。分别检查能力：

```sh
v4l2-ctl -d /dev/videoX --list-formats-ext
v4l2-ctl -d /dev/videoY --list-formats-ext
```

两路的 `4000x3000 MJPG` 都必须列出 4 fps 和 2 fps。

### 10.3 电脑端同时预览

先在板卡端设置一个组合，例如：

```text
fps 0 2
fps 1 4
```

然后在电脑端打开两个终端。

电脑终端一：

```sh
ffplay -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 2 /dev/videoX
```

电脑终端二：

```sh
ffplay -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 4 /dev/videoY
```

两路都显示画面后，回到板卡控制台输入：

```text
uvc-status all
capture-status all
```

通过条件：

- 两路 `host_streaming=1`；
- 两路 `sent` 持续增加；
- `encode_errors=0`、`last_error=0`、`last_mpp_error=0`；
- cam0 实测约 2 Hz，cam1 实测约 4 Hz；
- 两个画面来源正确，没有串路。

切换到其他组合前，先在电脑端按 `q` 退出两个 `ffplay`，板卡设置新帧率后，再按
新帧率重新打开。UVC描述符表示电脑可以选择2或4 fps，但电脑选择的帧率不会自动
修改IMX586；传感器帧率仍以板卡 `fps CAMERA_ID 2|4` 命令为准。

### 10.4 电脑端抓取对比图

以 cam0=2 Hz、cam1=4 Hz 为例：

```sh
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 2 \
  -i /dev/videoX -frames:v 1 cam0_2hz.jpg

ffmpeg -y -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 4 \
  -i /dev/videoY -frames:v 1 cam1_4hz.jpg

file cam0_2hz.jpg cam1_4hz.jpg
```

两张图片都应为4000x3000 JPEG，并且画面分别来自正确相机。帧率不能通过单张
图片判断，最终频率以板卡 `CAPTURE_STATUS fps_x1000` 和20秒连续UVC观察为准。

## 11. 结束测试

先关闭电脑端 `ffplay`，然后在板卡控制台输入：

```text
uvc-stop all
sync-stop
stream-stop all
quit
```

需要恢复开机常驻服务时，在板卡终端执行：

```sh
systemctl restart camera-uvc.service
systemctl status camera-uvc.service --no-pager
```

常驻服务会自动启动共享4 Hz XVS。不要再启动第二个 `camera_aiq_test`。

## 12. 常见失败及处理

### 12.1 `fps-xvs-query` 报错

原因通常是板卡仍运行旧内核，或程序没有找到正确的 IMX586 subdev。检查：

```text
status all
```

两路必须有 `xvs_config_valid=1` 和不同的 `sensor_device="/dev/v4l-subdevN"`。

### 12.2 `running=1`，但 `frames=0`

说明程序已启动采集，但传感器没有收到物理 XVS。检查：

- MCU是否真的执行了 `START,4000,10`；
- `sync-controller-status` 是否为 `frequency_millihz=4000`；
- 示波器能否在两颗 IMX586 的 XVS 输入脚看到同一个4 Hz脉冲；
- XVS是否空闲高、低有效；
- XVS电平是否符合模组要求。

### 12.3 `target fps did not stabilize`

检查共享 XVS 是否稳定为4 Hz。若 MCU 被错误设置为2 Hz，则：

- `xvs_input_thin=0` 只能得到约2 Hz；
- `xvs_input_thin=1` 只能得到约1 Hz；
- 四种独立组合必然失败。

### 12.4 `other_camera_continued=0`

确认测试前执行过 `stream-start all`。如果两路原本都在运行，仍然为0，则记录
完整命令输出、`capture-status all` 和内核日志，该项判定不通过。

### 12.5 UVC没有设备或没有画面

依次检查：

```sh
# 板卡端
uvc-status all
ls /sys/class/udc
ls /sys/kernel/config/usb_gadget/rockchip/functions/

# 电脑端
lsusb -t
v4l2-ctl --list-devices
```

电脑端应工作在 `5000M` 或更高的USB 3.x链路。若显示 `480M`，说明当前是USB 2.0，
需要检查线材、Hub、电脑接口和RK3576 USB角色配置。

## 13. 测试记录表

| 序号 | cam0设置/实测 | cam1设置/实测 | cam0 thin | cam1 thin | 未切换路持续计帧 | 参数保持 | UVC双路 | 结果 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 4 Hz /  | 4 Hz /  | 0 | 0 | 是/否 | 是/否 | 是/否 | 通过/不通过 |
| 2 | 2 Hz /  | 4 Hz /  | 1 | 0 | 是/否 | 是/否 | 是/否 | 通过/不通过 |
| 3 | 4 Hz /  | 2 Hz /  | 0 | 1 | 是/否 | 是/否 | 是/否 | 通过/不通过 |
| 4 | 2 Hz /  | 2 Hz /  | 1 | 1 | 是/否 | 是/否 | 是/否 | 通过/不通过 |

最终只有以下条件全部满足才能判定通过：四种组合实测频率正确、每次切换未操作路
持续计帧、两路曝光增益不串路、20轮切换无失败、双UVC都能按各自帧率出图。

