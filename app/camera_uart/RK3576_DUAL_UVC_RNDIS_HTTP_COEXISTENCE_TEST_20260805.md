# RK3576 双 UVC、RNDIS、HTTP 共存修改与自测说明

## 1. 本次目标

在当前未连接 MCU、没有外部 XVS 脉冲的条件下，同时保持：

- cam0 和 cam1 两路 UVC 输出；
- USB RNDIS 虚拟网口；
- 电脑可 ping `192.168.55.1`；
- 电脑可 SSH 登录 `root@192.168.55.1`；
- cam0 和 cam1 两路 HTTP 图像输出；
- 电脑停止或重新打开 UVC 取流时，RNDIS、SSH 和 HTTP 不掉线。

## 2. 原因与修改

旧启动镜像在两路 IMX586 的 DTS 中设置了：

```dts
sony,xvs-slave-mode;
sony,xvs-input-thin = <0>;
```

这会让传感器等待外部 XVS。当前未接 MCU，因此 ISP、UVC 和 HTTP 都已启动，
但源头没有任何图像帧。

本次只从以下两路 DTS 中移除了上述 XVS 启动属性：

- `kernel/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi0.dtsi`
- `kernel/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi1.dtsi`

IMX586 驱动中的 XVS 支持没有删除。USB Gadget、RNDIS、IP 地址、SSH、HTTP 和
UVC 配置均未修改。当前镜像让两颗传感器自由运行，适合无 MCU 联调。

另外，应用已使用统一常驻模式：

```text
/root/camera_uart/camera_aiq_test --all-daemon
```

同一进程只启动一次双路采集，并同时向 HTTP 和双 UVC 分发图像。RNDIS 的生命
周期由 `usbdevice.service` 管理。`uvc-stop` 只停止 UVC 编码和送帧，不解绑 USB
Gadget，也不删除 RNDIS 功能。

## 3. 镜像和备份

新 boot 镜像：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/kernel-6.1/boot.img
SHA-256: c0067819104d427a983328274576ad766ee7920364ef41d4a3808a7a0e02385d
```

本地修改前备份：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/backups/20260805_before_no_mcu_freerun/
```

板端修改前完整 boot 分区备份：

```text
/root/camera_uart/backup_20260805_before_no_mcu_freerun/boot_partition_before.img
SHA-256: 3df44dbab4903992f76dcc69d56f8433ac735dd6e2916b719567d512094cb6a5
```

刷写后已使用 `cmp` 对新镜像长度范围逐字节校验，结果一致。

## 4. 本次实测结果

| 项目 | 实测结果 | 结论 |
| --- | --- | --- |
| USB 复合设备 | `2207:0017 Rockchip rk3xxx` | 通过 |
| RNDIS | 电脑 `192.168.55.20/24`，板端 `192.168.55.1/24` | 通过 |
| 重启后持续 ping | 20 发 20 收，0% 丢包 | 通过 |
| SSH | 使用密码 `root` 实际登录成功 | 通过 |
| HTTP 端口 | `192.168.55.1:8080` 可连接 | 通过 |
| 双 HTTP 实流 | 12 秒约 90.7 MB / 80.8 MB | 通过 |
| UVC cam0 | 4000x3000 MJPEG，电脑请求 4 Hz，连续取得 8 帧 | 通过 |
| UVC cam1 | 4000x3000 MJPEG，电脑请求 2 Hz，连续取得 4 帧 | 通过 |
| UVC + HTTP + ping 并发 | 两路 UVC、两路 HTTP 同时工作，ping 12 发 12 收 | 通过 |
| UVC 客户端关闭后网络 | ping 10 发 10 收，SSH 22 端口正常 | 通过 |
| 服务稳定性 | `camera-uvc.service` active，`NRestarts=0` | 通过 |
| UDC | `configured`，`high-speed` | 通过 |
| XVS 硬件同步 | 当前无 MCU，未启用 | 待接 MCU |

### 4.1 最终共存复核

在不重启板端服务的条件下，同时打开双 UVC、双 HTTP，并持续 ping：

- cam0 UVC：4000x3000 MJPEG、4 Hz，连续取得 8 帧；
- cam1 UVC：4000x3000 MJPEG、2 Hz，连续取得 4 帧；
- cam0 HTTP：12 秒收到 91023920 字节，HTTP 200；
- cam1 HTTP：12 秒收到 80650767 字节，HTTP 200；
- 并发期间 ping：12 发 12 收，0% 丢包；
- 关闭两个 UVC 客户端后 ping：8 发 8 收，0% 丢包；
- 关闭 UVC 客户端后密码 SSH 实际登录成功；
- 关闭 UVC 客户端后两路 HTTP 各抓取一张 4000x3000 JPEG 成功；
- 最终 `usb0` 仍为 `UP`，UDC 仍为 `configured`，服务 `NRestarts=0`。

随后又执行了 60 秒双 UVC、双 HTTP 和 ping 并发负载。负载进程按设定结束后，
再次 ping 10 发 10 收、0% 丢包，密码 SSH 登录成功，两路 HTTP 再次各抓取一张
4000x3000 JPEG；三个服务仍为 `active`，`NRestarts=0`，UDC 仍为 `configured`。

最终复核图片保存在：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/test_results/20260805_final_coexistence_recheck/
```

说明：`high-speed` 表示当前经 USB 2.0/Hub 以 480 Mbps 枚举，不是 USB 3.0
SuperSpeed。本次 4000x3000 双 UVC、双 HTTP 和 RNDIS 并发短时测试已通过，但产
品长时间满负载仍建议使用 USB 3.0 直连。

测试图片保存在：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart/test_results/20260805_no_mcu_freerun_all_outputs/
```

其中：

- `uvc_cam0_01.jpg` 到 `uvc_cam0_08.jpg`：cam0 UVC 帧；
- `uvc_cam1_01.jpg` 到 `uvc_cam1_04.jpg`：cam1 UVC 帧；
- `http_cam0.jpg`、`http_cam1.jpg`：两路 HTTP 帧。

## 5. 小白自测步骤

### 5.1 接线和启动

1. RK3576 使用独立电源供电。
2. 用支持数据传输的 USB 线把板卡 Device/OTG Type-C 口接到电脑。
3. 等待约 20 秒，不需要手工执行 SSH 端口转发。

### 5.2 电脑端验证网口

电脑终端执行：

```bash
ip -brief address
ping -c 20 192.168.55.1
```

合格标准：电脑出现一个 `192.168.55.x/24` 的 USB 网卡；ping 20 次全部收到。

### 5.3 电脑端验证 SSH

```bash
ssh root@192.168.55.1
```

密码：

```text
root
```

能出现 `root@localhost:~#` 即为通过。

### 5.4 电脑端验证 HTTP

浏览器直接打开：

```text
http://192.168.55.1:8080/
```

不需要运行 `ssh -L`。页面中 cam0、cam1 都应持续更新。

也可在电脑终端各抓一张图：

```bash
ffmpeg -y -i http://192.168.55.1:8080/cam0 -frames:v 1 cam0_http.jpg
ffmpeg -y -i http://192.168.55.1:8080/cam1 -frames:v 1 cam1_http.jpg
```

### 5.5 电脑端确认 UVC 节点

```bash
v4l2-ctl --list-devices
```

本次枚举中 cam0 的有效采集节点为 `/dev/video0`，cam1 为 `/dev/video2`。
重新插拔后编号可能变化，应以 `v4l2-ctl --list-devices` 的实际结果为准。

查看节点格式：

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video2 --list-formats-ext
```

两路都应显示 MJPEG、4000x3000、2 fps 和 4 fps。

### 5.6 电脑端同时验证双 UVC

终端一打开 cam0，向 UVC 请求 4 Hz：

```bash
ffplay -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 4 /dev/video0
```

终端二打开 cam1，向 UVC 请求 2 Hz：

```bash
ffplay -f v4l2 -input_format mjpeg -video_size 4000x3000 -framerate 2 /dev/video2
```

两个窗口都有画面即为双 UVC 通过。关闭任意一个 ffplay 只会停止该电脑端 UVC
取流，不应影响 USB 网口。

### 5.7 验证 UVC 不影响 ping、SSH 和 HTTP

保持两个 UVC 窗口运行，在终端三执行：

```bash
ping 192.168.55.1
```

在终端四执行：

```bash
ssh root@192.168.55.1
```

同时刷新 `http://192.168.55.1:8080/`。合格标准是两路 UVC 有画面、HTTP 两路
有画面、ping 无丢包且 SSH 可登录。随后关闭两个 UVC 窗口，再确认 ping 和 HTTP
仍然工作。

### 5.8 板端检查服务

SSH 登录板端后执行：

```bash
systemctl is-active usbdevice.service camera-uvc.service ssh
systemctl show camera-uvc.service -p NRestarts -p ActiveState -p SubState
cat /sys/class/udc/23000000.usb/state
ip -brief address show usb0
```

合格结果应包含：

```text
active
active
active
NRestarts=0
ActiveState=active
SubState=running
configured
usb0 UP 192.168.55.1/24
```

## 6. 当前边界与后续接 MCU

当前是无 MCU 自由运行镜像，因此：

- 双 UVC 的 4 Hz/2 Hz是电脑端 UVC 协商和应用送帧节流结果；
- 现在不能据此宣称两颗传感器已实现外部 XVS 硬件同步；
- XVS 示波器相位、MCU 脉冲计数和帧 ID 对齐仍需接 MCU 后验收。

接入 MCU 并确认其能够持续输出 1.8 V、低有效、4 Hz XVS 后，再把两路 DTS 恢复为：

```dts
sony,xvs-slave-mode;
sony,xvs-input-thin = <0>;
```

然后重新编译并刷写 boot 镜像，服务改为带 MCU 参数启动：

```text
/root/camera_uart/camera_aiq_test --all-daemon --uart /dev/ttyS9 --sync-timer-hz 1000000 --xvs-autostart-hz 4 --xvs-low-pulse-us 10
```

在 MCU 脉冲已稳定之前不要刷回 XVS 从模式，否则传感器会再次等待脉冲，UVC 和
HTTP 都没有图像。这个切换不改变 RNDIS、SSH、HTTP 或 UVC Gadget 配置。

## 7. 回滚方法

如新 boot 无法正常启动，可从串口登录板端后恢复完整备份：

```bash
dd if=/root/camera_uart/backup_20260805_before_no_mcu_freerun/boot_partition_before.img of=/dev/mmcblk0p3 bs=4M conv=fsync
reboot
```

不要在镜像来源或分区号不确定时执行此命令。
