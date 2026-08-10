# RK3576 双 UVC 与 RNDIS 生命周期修复记录

日期：2026-08-04  
对象：RK3576 + 双 IMX586 + `camera_aiq_test`  
目标：停止或恢复 UVC 视频时，USB 网卡和网口图像输出不掉线。

## 1. 问题与边界

复合 USB 同时包含 `rndis.0`、`uvc.0` 和 `uvc.1`。整个 Gadget 由
`usbdevice.service` 创建并绑定到 UDC；相机程序只负责打开两个 UVC 输出节点并送帧。

旧程序执行 `uvc-stop all` 时还会退出 UVC 控制层、关闭两个 UVC 节点。它没有删除
configfs 或解绑 UDC，但会造成主机侧 UVC 控制端点状态变化，存在连带影响复合设备的
风险。正确做法是把“暂停视频”和“销毁程序资源”分开。

网络也分为两层：

- RNDIS/`usb0` 属于 `usbdevice.service`，应独立常驻。
- `http://192.168.55.1:8080/cam0` 和 `/cam1` 属于相机程序，只有程序运行且执行
  `net-start 0/1` 后才提供图像。

因此，UVC 软停止必须同时保留 RNDIS 和 HTTP；程序退出时 RNDIS 保留，但 HTTP 随
程序结束，这是预期边界。

## 2. 修改内容

修改文件：

- `camera_uvc_backend.cpp`
- `camera_uvc_backend.h`
- `camera_aiq_test.cpp`

行为变化：

1. `camera_uvc_stop()` 仅停止 cam0/cam1 的编码线程和送帧队列。
2. UVC 控制线程、回调和两个 UVC 文件描述符继续保留。
3. 真正的 UVC 控制层退出只在 `camera_uvc_destroy()` 中执行。
4. 命令成功时明确打印
   `OK command=uvc-stop target=all usb_gadget=kept rndis=kept`。
5. `uvc-start all` 可在软停止后直接恢复，无需重新枚举 USB。

没有修改内核、DTS、configfs 脚本、IP 地址或 HTTP/UVC 编码参数。

## 3. 构建与部署

新程序 SHA256：

```text
543a6bc8ece1310ac7c00ffa3b1d8defdd67c33d5bae2b05dcc36d3d4e3a64cb
```

板端程序：`/root/camera_uart/camera_aiq_test`  
板端旧版备份：
`/root/camera_uart/backup_20260804_before_uvc_rndis_lifecycle/camera_aiq_test`

本地完整备份：
`backups/20260804_before_uvc_rndis_lifecycle/`

## 4. 已完成测试

| 测试 | 结果 |
|---|---|
| AArch64 交叉编译 | 通过 |
| UART 协议、XVS UART、时间同步、阶段 6/7 主机自检 | 全部通过 |
| 板端同步绑定、JPEG/EXIF、UART 协议自检 | 全部通过 |
| `stream-start all`、`uvc-start all` | 成功 |
| `uvc-stop all` 后 UDC/configfs/RNDIS 保留 | 通过 |
| `uvc-stop all` 后双 HTTP `server_running=1` | 通过 |
| 软停止后再次 `uvc-start all`，不重新打开 UVC 节点 | 通过 |
| 退出相机程序后 `usbdevice.service` 和 RNDIS 保留 | 通过 |

本轮没有 MCU/XVS 输入，因此两路采集帧数为 0；这不影响生命周期验证，但不能用来
证明 UVC/HTTP 的真实图像连续性。当前电脑端也没有建立 RNDIS carrier，未完成修改后
的连续 ping 实测。

## 5. 恢复物理连接后的最终验收

电脑端保持运行：

```bash
ping -i 0.2 192.168.55.1
```

相机程序中执行：

```text
stream-start all
net-start 0
net-start 1
uvc-start all
wait 3000
uvc-stop all
wait 10000
uvc-start all
wait 3000
uvc-status all
net-status all
```

板端另一个终端检查：

```bash
systemctl is-active usbdevice.service
cat /sys/kernel/config/usb_gadget/rockchip/UDC
ls -l /sys/kernel/config/usb_gadget/rockchip/configs/b.1/
ip -br address show usb0
```

通过标准：

- 停止和恢复 UVC 期间，电脑的 RNDIS 网卡不消失、不重新获取地址，持续 ping 无丢包。
- UDC 始终有控制器名称，三个功能链接始终存在。
- 双 HTTP 客户端持续取流，`net-status all` 的发送计数继续增加且错误为 0。
- 恢复 UVC 后电脑两路 UVC 继续出图，不需重新插拔 USB。

正常使用中不得执行 `usbdevice stop`、`usbdevice restart` 或手工解绑 UDC。这些命令
操作的是整个复合 USB，必然同时影响 RNDIS 和双 UVC。
