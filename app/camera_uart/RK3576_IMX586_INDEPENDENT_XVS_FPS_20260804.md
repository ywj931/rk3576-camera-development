# RK3576 双 IMX586 独立 2/4 Hz 切换说明与测试

日期：2026-08-04

## 1. 结论

之前设备树中固定 `cam0 input-thin=0`、`cam1 input-thin=1`，只是为了验收
`cam0=4 Hz、cam1=2 Hz` 这一种组合。它不能满足两路自由切换的最终需求。

现在修改为：

- MCU 始终给两颗 IMX586 共用的 XVS 线输出连续 4 Hz、低有效 10 us 脉冲；
- cam0 和 cam1 上电都默认接收每个 XVS 脉冲，即 4 Hz；
- 每颗 IMX586 都可以单独设置 `0x3F6F`：`0` 接收每个脉冲（4 Hz），
  `1` 接收每两个脉冲中的一个（2 Hz）；
- `fps 0 2|4` 只改变 cam0，`fps 1 2|4` 只改变 cam1；
- 两个 UVC 功能都向电脑声明 4000x3000 MJPEG 的 4 fps 和 2 fps；
- 曝光、增益、ISO、保存和 UVC 队列仍按 camera_id 分开，帧率切换不会改另一
  路参数。

四种组合全部支持：

| cam0 | cam1 | cam0 input-thin | cam1 input-thin |
| --- | --- | --- | --- |
| 4 Hz | 4 Hz | 0 | 0 |
| 4 Hz | 2 Hz | 0 | 1 |
| 2 Hz | 4 Hz | 1 | 0 |
| 2 Hz | 2 Hz | 1 | 1 |

## 2. 为什么不能分别让 MCU 输出 4 Hz 和 2 Hz

当前硬件只有一根共享 XVS 线，MCU 对两颗传感器只能输出同一组边沿。若 MCU
把共享 XVS 改成 2 Hz，两颗相机都会同时只收到 2 Hz，不能独立。

所以共享时基必须保持最高需求 4 Hz，再由每颗 IMX586 自己决定接收全部脉冲
还是隔一个接收一次。这个结构才能让 cam0/cam1 独立选择 2/4 Hz。

该硬件结构只能得到 2 Hz 和 4 Hz 两档。如果以后要求任意频率，或要求某一路
切换时自身也完全不断流，需要两根独立 XVS、额外同步逻辑或传感器支持运行中
无缝修改；当前一根共享 XVS 做不到任意频率。

## 3. 为什么切换目标相机会短暂停一下

IMX586 手册要求 `0x3F6E/0x3F6F` 只能在软件待机状态修改。因此程序切换某一路
时按以下顺序执行：

1. 读取并保存目标相机当前曝光和增益；
2. 只停止目标相机的 V4L2/RKAIQ；
3. 通过目标 IMX586 subdev ioctl 设置并回读 XVS input-thin；
4. 更新目标 UVC 源帧率；
5. 只重启目标相机，并恢复该路曝光和增益；
6. 等待实测帧率稳定，同时检查另一条相机的帧计数仍在增加。

因此允许被切换的目标路短暂停流；未切换的一路不允许停止、重启或停止计帧。

## 4. 修改位置

- `kernel/drivers/media/i2c/imx586.c`：增加 XVS 分频 set/get ioctl，要求待机修改。
- `kernel-6.1/include/uapi/linux/rk-camera-module.h`：定义两个 ioctl 命令。
- `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi0.dtsi`：cam0 默认 4 Hz。
- `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi1.dtsi`：cam1 也默认 4 Hz。
- `app/camera_uart/camera_backend.cpp`：自动找到两颗 IMX586 subdev 并独立读写。
- `app/camera_uart/camera_aiq_test.cpp`：`fps` 命令完成单路停机、设置、恢复和隔离检查。
- `app/camera_uart/camera_uvc_backend.cpp`：两路 UVC 源帧率分别维护。
- `external/rkscript/usbdevice`、`external/uvc_app/uvc/uvc-gadget.c`：两路 UVC
  均声明 4 fps（2500000）和 2 fps（5000000）。

修改前文件备份在：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/backups/20260804_before_independent_xvs_fps/
```

## 5. 上板前提

必须同时更新新 `boot.img` 和新 `camera_aiq_test`。只更新应用、不更新内核时，
`fps` 会返回 `fps-xvs-query` 错误，不会假装设置成功。

板卡进入 XVS 从模式后，没有外部 XVS 脉冲就不会出帧。正式测试前必须确认：

- MCU 已连接共享 XVS；
- XVS 空闲为高、脉冲低有效，连续 4 Hz，低电平约 10 us；
- MCU 的 4 Hz 输出在相机开始采集后保持运行，切换单路时不能停止；
- `/dev/ttyS9` 为 115200、8N1，MCU 能响应程序发出的 4 Hz XVS 启动命令。

## 6. 板端测试

启动已安装的 UVC 服务后进入程序控制终端，先执行：

```text
status all
capture-status all
uvc-status all
```

`status all` 两路都应出现：

```text
xvs_config_valid=1
sensor_device="/dev/v4l-subdevN"
```

其中 4 Hz 应回读 `xvs_input_thin=0`，2 Hz 应回读
`xvs_input_thin=1`。若 `xvs_config_valid=0` 或 `fps-xvs-query` 报错，说明板卡
仍在使用旧内核、subdev 匹配错误或驱动 ioctl 不可用，不能继续判定帧率通过。

按顺序验证全部四种组合：

```text
fps 0 4
fps 1 4
capture-status all
status all

fps 1 2
capture-status all
status all

fps 0 2
fps 1 4
capture-status all
status all

fps 1 2
capture-status all
status all
```

每条 `fps` 成功后程序会输出：

```text
OK command=fps ... fps_stable=1 ... other_camera_continued=1
```

验收要求：

- 4 Hz 的 `measured_fps_x1000` 在 3800～4200；
- 2 Hz 的 `measured_fps_x1000` 在 1900～2100；
- `xvs_input_thin` 和所设帧率对应；
- 每次切换都出现 `other_camera_continued=1`；
- 未操作那一路的 `frames` 持续增加，曝光/增益/ISO 回读不变化；
- 不允许出现 `sequence_drops` 持续增加、`last_errno` 非 0 或 AIQ 错误。

## 7. 反复切换与隔离测试

下面顺序至少循环 20 次：

```text
fps 0 2
fps 1 4
fps 0 4
fps 1 2
```

然后执行：

```text
status all
capture-status all
uvc-status all
```

通过条件是 80 次单路切换全部返回 `OK`，每次
`other_camera_continued=1`，两路最终仍能分别回读正确的 XVS 分频、曝光和增益。

## 8. 电脑端 UVC 测试

USB 重新枚举后，先找到两路 RK3576 UVC 视频节点，再分别执行：

```sh
v4l2-ctl -d /dev/videoX --list-formats-ext
v4l2-ctl -d /dev/videoY --list-formats-ext
```

两路 4000x3000 MJPEG 都必须列出 `4.000 fps` 和 `2.000 fps`。然后板端先执行
目标 `fps` 命令，电脑端用相同帧率打开对应 UVC 节点。例如板端：

```text
fps 0 2
fps 1 4
```

电脑端：

```sh
ffmpeg -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 2 -i /dev/videoX -t 20 -f null -
ffmpeg -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 4 -i /dev/videoY -t 20 -f null -
```

UVC 描述符的 2/4 fps 是电脑可以协商的能力，不等于传感器会被电脑自动改帧率。
传感器帧率仍以板端/UART 的 `fps CAMERA_ID 2|4` 命令为准，电脑端应选择与板端
相同的帧率。

## 9. 当前已完成和仍需实测的边界

已完成：应用交叉编译、内核编译、DTB 两路 XVS/默认 4 Hz 检查、两路 UVC
2/4 fps 描述符检查、UART 协议、PPS/UTC、EXIF 和 trigger/frame_id 软件回归测试。

仍需 MCU/XVS 硬件到位后上板完成：四种组合实际帧率、20 轮反复切换、示波器
4 Hz XVS 验证、目标路短暂停流时间、未切换路连续性和双 UVC 电脑端实流测试。
