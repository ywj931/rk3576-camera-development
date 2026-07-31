# RK3576 + IMX586 UVC Type-C 不枚举排查记录（2026-07-25）

## 1. 结论

相机采集、RKAIQ、UVC 应用、ConfigFS UVC 描述符和 DWC3 Device 模式均已
分别验证。当前失败点位于 **USB 主机开始枚举之前**：板端 UDC 一直为
`not attached`，电脑端没有出现新的 USB 设备，也没有 `/dev/videoX`。

本次还用修改前的 boot 镜像做了 A/B 测试。修改前内核同样显示
`not attached / UNKNOWN`，电脑同样不枚举，所以本次固定 Device 的 DTS 修改
不是造成故障的原因。

当前应优先检查 Type-C 的 VBUS、CC、D+、D- 和 FUSB302/I2C 硬件链路，继续
修改相机应用或 UVC 编码程序不会解决“电脑完全看不到 USB 设备”的问题。

## 2. 正常的数据链路

```text
IMX586 camera0
  -> RKISP/RKAIQ
  -> /dev/video22，4000x3000 NV12M
  -> camera_aiq_test
  -> RK3576 MPP MJPEG
  -> /dev/video49（UVC gadget 端）
  -> DWC3 USB Device
  -> Type-C D+/D-
  -> 电脑 USB Host
  -> 电脑 /dev/videoX
```

本次已经验证到 `/dev/video49`，失败发生在 DWC3 与电脑 USB Host 之间。

## 3. 当前 SDK 修改

设备树文件：

```text
/home/ywj/rk3576_sdk/TaishanPi-3-Linux/kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-typec.dtsi
```

固定 UVC Device 模式的主要设置：

```dts
&usb_drd0_dwc3 {
	dr_mode = "peripheral";
	maximum-speed = "high-speed";
	phys = <&u2phy0_otg>;
	phy-names = "usb2-phy";
	status = "okay";
};

&u2phy0_otg {
	/delete-property/ rockchip,typec-vbus-det;
	rockchip,sel-pipe-phystatus;
	rockchip,dis-u2-susphy;
	rockchip,vbus-always-on;
	status = "okay";
};
```

同时删除不可用的 FUSB302 角色切换依赖，并禁止 USB3/DP PHY，先只验证
USB2 High-Speed UVC。

当前构建镜像：

```text
kernel-6.1/boot.img
SHA-256: acf66bc7a9b8c2eb53648071db4653bf19c6e344301a5add7983e975dfcb4c8b
```

源码修改前备份：

```text
kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-typec.dtsi.before_phy_fix_20260725
```

板端 boot 备份：

```text
/root/camera_uart/boot_backup/boot_before_fixed_uvc_20260725.img
SHA-256: 0c2d919f388d4d96a052dc390652c14ceac170581bd94595c0b81c156aa646d9

/root/camera_uart/boot_backup/boot_current_phy_fixed_20260725.img
SHA-256: 5fde9077fd6c45aa3e45370f10352fd5be084a78c7e5321e5be340f4466cce54
```

第二个文件是当前 eMMC boot 分区的完整 64 MiB 备份，可直接恢复。

## 4. 相机和 UVC 应用实测

板端运行同一个已有程序：

```text
/root/camera_uart/camera_aiq_test
```

程序内执行：

```text
stream-start 0
wait 2000
capture-status 0
uvc-start 0
wait 5000
uvc-status
```

实测相机采集：

```text
size=4000x3000
format=NV12M
fps_x1000=30020～30043
frames=1078
sequence_drops=0
last_errno=0
device=/dev/video22
```

实测 UVC 应用：

```text
uvc-start 成功
UVC gadget 节点=/dev/video49
配置=4000x3000@10fps/MJPEG
host_streaming=0
negotiated=0x0@0fps/none
submitted=0
encoded=0
sent=0
skipped_no_host=1021
encode_errors=0
last_mpp_error=0
```

这里 `skipped_no_host` 增长的含义是相机帧持续到达，但电脑没有发出 UVC
`STREAMON`，所以应用正确地跳过编码和发送。

## 5. DWC3/PHY 实测

固定 Device 内核下、UVC 应用已启动时：

```text
UDC state      = not attached
UDC speed      = UNKNOWN
GCTL           = 0x30c12004（Device 模式）
GUSB2PHYCFG    = 0x00101408（USB2 PHY 未软复位、未 suspend）
DCTL           = 0x80f00000（RUN_STOP=1）
DSTS           = 0x00020001
GEVNTCOUNT     = 0（没有收到主机 RESET/枚举事件）
PHY LINESTATE  = 0（SE0）
```

这说明 UDC 已运行并请求连接，但 USB2 PHY 没看到有效的主机侧总线状态。

## 6. 最小 ACM 排除测试

为排除相机、MJPEG 和 UVC 描述符，临时创建了只有一个 ACM 串口功能的最小
ConfigFS gadget：

```text
VID:PID = 1d6b:0104
product = RK3576-ACM-test
function = acm.usb0
```

它可以正常绑定 `23000000.usb`，但结果仍为：

```text
板端：not attached / UNKNOWN
电脑：无 1d6b:0104，无 /dev/ttyACM*
```

因此故障与 UVC 类、相机代码、AIQ 或 MPP 编码无关。测试后临时 ACM gadget
已删除，正常 `rockchip` UVC gadget 已恢复绑定。

## 7. 修改前/修改后 A/B

### 修改后固定 Device 镜像

```text
UDC=not attached
speed=UNKNOWN
电脑 lsusb 无 RK3576
```

### 修改前 OTG/FUSB302 镜像

```text
内核 Linux 6.1.99 #5
FUSB302 节点存在并绑定 typec_fusb302 驱动
i2cget -f -y 2 0x22 0x01 -> Read failed
UDC=not attached
speed=UNKNOWN
电脑 lsusb 无 RK3576
```

修改前内核的 DWC3 在没有完成 OTG 角色切换时处于未上电状态。诊断中直接访问
该未上电寄存器触发了 ARM64 Asynchronous SError，板卡需要断电重启后恢复当前
boot 备份。这是旧内核调试限制，不是相机程序崩溃。

## 8. 下一步硬件检查顺序

### 8.1 先换线和电脑端口

1. 板卡使用独立 DC 电源。
2. 使用确认支持 USB 2.0 数据的短 USB-A 转 Type-C 线。
3. 直接接电脑主板 USB 2.0/3.0 口，不经过 Hub、显示器或扩展坞。
4. Type-C 插头正反各测试一次。
5. 板端启动 gadget 后，电脑执行 `lsusb`，板端查看 UDC `state`。

通过标志是板端从 `not attached` 进入 `powered/default/addressed/configured`，
电脑 `lsusb` 出现 RK3576 gadget。只有到这一步以后才需要测试 `/dev/videoX`。

### 8.2 测量 Type-C VBUS

Type-C 插到电脑后，用万用表测连接器 VBUS 对 GND，应约为 5 V。若没有 5 V：

- 换 USB 数据线和电脑 USB 口；
- 检查 Type-C 连接器焊接、保险丝/限流器件和 VBUS 走线；
- 检查 CC 上的设备端 Rd 或 FUSB302 配置。

### 8.3 检查 D+/D-

在 gadget 已绑定且 UVC 应用已执行 `uvc-start 0` 时检查：

- Type-C 连接器 D+、D- 到 RK3576 USB2 OTG DP/DM 的通断；
- 中间 0 欧电阻、共模电感、ESD 保护器件是否开路或短路；
- D+ 与 D- 是否短路到地、VBUS 或彼此短路；
- 示波器观察插入时是否出现主机 RESET 和枚举波形。

当前 PHY `LINESTATE=0` 且电脑完全无设备，D+/D- 连续性是最高优先级检查项。

### 8.4 修复 FUSB302/I2C2

FUSB302 设备树地址为 I2C2 的 `0x22`，但寄存器读取失败。依次检查：

1. FUSB302 供电是否正常；
2. I2C2 SCL/SDA 空闲时是否为高电平；
3. SCL/SDA 是否被某器件持续拉低；
4. `0x22` 地址和芯片焊接方向；
5. 中断 GPIO0_B4 是否异常持续触发；
6. 修复后确认 `i2cget -f -y 2 0x22 0x01` 可以读到寄存器。

如果客户最终只把这个口当 UVC Device，可以保持固定 `peripheral` 模式；但
Type-C 的 CC/Rd 和 USB2 D+/D- 物理链路仍必须正常。

## 9. 恢复当前 boot 镜像

板卡断电重启、登录后执行：

```bash
dd if=/root/camera_uart/boot_backup/boot_current_phy_fixed_20260725.img \
   of=/dev/mmcblk0p3 bs=4M conv=fsync status=progress
sha256sum /dev/mmcblk0p3
reboot
```

预期完整分区 SHA-256：

```text
5fde9077fd6c45aa3e45370f10352fd5be084a78c7e5321e5be340f4466cce54
```

恢复启动后，再确认：

```bash
cat /sys/kernel/config/usb_gadget/rockchip/UDC
cat /sys/class/udc/23000000.usb/state
```

## 10. 端到端验收条件

完成物理链路修复后，按以下条件验收：

- 电脑 `lsusb` 能看到 RK3576 gadget；
- 电脑出现 `/dev/videoX`；
- 板端 UDC 为 `configured`，速度至少 `high-speed`；
- `camera_aiq_test` 的 `uvc-status` 显示 `host_streaming=1`；
- `negotiated=4000x3000@5fps/MJPEG` 或 `10fps/MJPEG`；
- `submitted/encoded/sent` 持续增长；
- `encode_errors=0`、`last_mpp_error=0`；
- 电脑能抓取并解码一张 4000x3000 JPEG。

参考资料：

- `docs/Common/USB/Rockchip_Developer_Guide_USB_EN.pdf`
- https://wiki.lckfb.com/zh-hans/tspi-3-rk3576/open-source-hardware/

