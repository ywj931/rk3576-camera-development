# RK3576 双 IMX586 阶段 4：UART9 实现与验证报告

日期：2026-07-20  
平台：泰山派 RK3576，Linux 6.1.99，aarch64  
摄像头：两路 Sony IMX586  
业务串口：`/dev/ttyS9`，115200 8N1，无软硬件流控  
调试串口：电脑 `/dev/ttyUSB0` 对应板端 `ttyFIQ0`，1500000 8N1

## 1. 本次结论

UART9 的内核启用、引脚复用、应用层参数配置和控制器内部收发均已通过。

| 验证项 | 结果 | 证据 |
| --- | --- | --- |
| 设备节点 | 通过 | 板端存在 `/dev/ttyS9` |
| 设备树状态 | 通过 | `/serial@2adc0000/status` 为 `okay` |
| UART 控制器 | 通过 | `ttyS9` 对应 `2adc0000.serial`、16550A、IRQ 42 |
| 引脚复用 | 通过 | 当前组为 `uart9m1-xfer` |
| RX/TX 引脚 | 通过 | `GPIO3_B2`、`GPIO3_B3` 均由 `2adc0000.serial` 占用 |
| 串口参数 | 通过 | 115200、CS8、无校验、1 停止位、无 RTS/CTS、RAW |
| 设备占用 | 通过 | 测试前没有其他进程占用 `/dev/ttyS9` |
| 控制器内部回环 | 通过 | TX 41 字节、RX 41 字节，内容一致，返回 0 |
| 连续内部回环 | 通过 | 连续运行 100 次，0 次失败，累计双向各 4100 字节 |
| 板端主动发送 | 通过到驱动层 | 写入 26 字节成功，UART9 TX 计数由 83 增加到 109 |
| HOT_SHOE1 外部双向通信 | 待接线 | 电脑当前只有调试 CH340，没有连接第二个 3.3 V USB-TTL |

内部回环通过说明 UART IP、Linux 串口驱动、`termios` 配置以及发送/接收数据路径正常。它不经过芯片外部焊盘，所以不能证明 HOT_SHOE1 的电压、焊接、线缆和电脑 USB-TTL 正常。外部测试仍需实际接线。

## 2. 为什么现在会出现 `/dev/ttyS9`

RK3576 内核原本已经包含通用 8250/DW UART 驱动，但板级设备树没有启用供相机控制使用的 UART9。此次在板级 DTS 中启用了 UART9，并指定 M1 引脚组：

```dts
&uart9 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart9m1_xfer>;
	status = "okay";
};
```

启动时发生的过程是：

```text
设备树 uart9=okay
  -> 内核探测 2adc0000.serial
  -> 8250/DW UART 驱动注册端口 9
  -> 创建 /dev/ttyS9
  -> pinctrl 把 GPIO3_B2/B3 切换成 UART9 RX/TX
```

设备树负责“启用哪个控制器、使用哪组引脚”，不负责长期固定 115200 波特率。115200 8N1 由打开串口的应用通过 `termios` 设置，这样每次服务启动都能得到确定参数。

## 3. 实际硬件映射

| 信号 | RK3576 引脚 | 设备树组 | 板卡接口 |
| --- | --- | --- | --- |
| UART9 RX | GPIO3_B2，pin 106 | `uart9m1-xfer` | `UART_CAM1_RX` / HOT_SHOE1 |
| UART9 TX | GPIO3_B3，pin 107 | `uart9m1-xfer` | `UART_CAM1_TX` / HOT_SHOE1 |

本次只启用 RX/TX，没有启用 RTS/CTS。应用也关闭了硬件流控和 XON/XOFF 软件流控。

运行时 pinctrl 实测：

```text
pin 106 (gpio3-10): 2adc0000.serial function uart9 group uart9m1-xfer
pin 107 (gpio3-11): 2adc0000.serial function uart9 group uart9m1-xfer
device: 2adc0000.serial current state: default
```

## 4. 修改的源码

### 4.1 内核设备树

文件：

```text
kernel/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576.dts
```

修改内容：

1. 启用 `uart9`。
2. 选择 `uart9m1_xfer`，对应 HOT_SHOE1 的 GPIO3_B2/B3。
3. 不启用 RTS/CTS。
4. 保留板卡原有 USB2 peripheral/RNDIS 行为，避免重新编译整个 DTS 时被 Type-C include 改成另一套 OTG 配置。

最终 DTS 已重新编译通过：

```sh
make -C kernel-6.1 -j17 ARCH=arm64 \
  CROSS_COMPILE="$PWD/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-" \
  rockchip/tspi-3m-rk3576.dtb
```

### 4.2 独立应用层测试工具

目录：

```text
app/camera_uart/
├── Makefile
├── README.md
└── camera_uart_test.c
```

该工具独立于 RKAIQ，不会绑定当前的一套 IQ 服务，也不会妨碍以后改成两个独立 IQ 程序。它提供：

- `loop`：UART 控制器内部回环，不需要外部短接；
- `send`：从 UART9 TX 主动发送指定文本；
- `echo`：收到数据后原样从 TX 发回，用于外部双向测试。

串口配置代码做了以下设置：

```text
输入速度 = B115200
输出速度 = B115200
字符长度 = CS8
PARENB = 0       无校验
CSTOPB = 0       1 个停止位
CRTSCTS = 0      无硬件流控
IXON/IXOFF = 0   无软件流控
ICANON/ECHO = 0  RAW 非规范模式、不回显
CLOCAL/CREAD = 1 忽略 modem 控制线、允许接收
```

工具退出时会恢复打开串口前的 `termios` 参数。

## 5. 编译与使用

### 5.1 在 RK3576 上编译

```sh
cd app/camera_uart
make clean all
```

板端已有 GCC 时也可直接执行：

```sh
gcc -O2 -Wall -Wextra camera_uart_test.c -o camera_uart_test
```

### 5.2 内部回环

```sh
./camera_uart_test -d /dev/ttyS9 loop
```

本次目标板实测输出：

```text
TX_BYTES=41 RX_BYTES=41
INTERNAL_LOOPBACK_PASS
```

返回值为 0。随后 `/proc/tty/driver/serial` 显示：

```text
9: uart:16550A mmio:0x2ADC0000 irq:42 tx:83 rx:83
```

此前还用临时版本发送/接收 42 字节并通过，因此累计计数为 83。

随后连续执行 100 次正式工具回环，结果为：

```text
LOOP_RUNS=100 FAILURES=0
TX_BYTES=41 RX_BYTES=41
INTERNAL_LOOPBACK_PASS
```

100 次共发送和接收各 4100 字节；UART9 计数从 `tx:109/rx:83` 增加到 `tx:4209/rx:4183`，增量与预期完全一致。

### 5.3 板端主动发送

```sh
./camera_uart_test -d /dev/ttyS9 send RK3576_UART9_TX_115200_8N1
```

本次实测：

```text
TX_BYTES=26
send_rc=0
```

驱动计数变化：

```text
发送前：ttyS9 tx:83  rx:83
发送后：ttyS9 tx:109 rx:83
```

该结果证明应用已把 26 字节交给 UART9 并完成发送。没有外部接收器时，不能据此证明电脑端实际收到电气信号。

## 6. 外部双向测试怎么接

需要一个独立的 **3.3 V TTL** USB 转串口模块。不能使用 RS-232 电平，也不要接 USB-TTL 的 VCC。

```text
电脑第二个 USB-TTL TX  -> HOT_SHOE1 UART_CAM1_RX / GPIO3_B2
电脑第二个 USB-TTL RX  <- HOT_SHOE1 UART_CAM1_TX / GPIO3_B3
电脑第二个 USB-TTL GND -- RK3576 GND
```

TX 和 RX 必须交叉，双方必须共地。

电脑现有 `/dev/ttyUSB0` 是板卡调试控制台，参数为 1500000 8N1，对应板端 `ttyFIQ0`。它不是 UART9 的对端。插入第二个 USB-TTL 后，电脑通常会新增 `/dev/ttyUSB1`，应根据 `lsusb` 和设备出现顺序确认，不能只凭编号猜测。

### 6.1 一次完成电脑发、板端收、板端发、电脑收

在板端通过调试控制台运行：

```sh
./camera_uart_test -d /dev/ttyS9 echo
```

看到：

```text
ECHO_READY (press Ctrl+C to stop)
```

电脑端打开第二个 USB-TTL：

```sh
picocom -b 115200 --databits 8 --parity n --stopbits 1 \
  --flow n /dev/ttyUSB1
```

电脑输入一串具有唯一性的文本，例如：

```text
PC_TO_RK3576_UART9_001
```

正确结果：

1. 板端打印非零的 `ECHO_BYTES`；
2. 电脑端收到完全相同的文本；
3. `/proc/tty/driver/serial` 中 ttyS9 的 TX 和 RX 都增加相同字节数。

这一个 echo 测试同时覆盖两个方向：

```text
电脑 TX -> UART9 RX -> 板端应用 -> UART9 TX -> 电脑 RX
```

### 6.2 单独验证板端 TX

先让电脑端监听 `/dev/ttyUSB1`，再在板端执行：

```sh
./camera_uart_test -d /dev/ttyS9 send RK3576_TX_TEST_001
```

电脑必须收到完整文本。若板端 `TX_BYTES` 成功、驱动 TX 计数增长但电脑仍收不到，依次检查：

1. 电脑监听的是否是第二个 USB-TTL，而不是调试口 `/dev/ttyUSB0`；
2. USB-TTL RX 是否接到 HOT_SHOE1 的 `UART_CAM1_TX`；
3. 是否共地；
4. USB-TTL 是否为 3.3 V TTL；
5. 电脑端是否确实为 115200 8N1、无流控；
6. 用示波器或逻辑分析仪检查 GPIO3_B3 发送时是否有 115200 波特率波形。

## 7. 阶段 4 程序应该在哪一层

最终 UART 相机控制程序属于 Linux **应用层常驻服务**。内核层只提供：

- UART9 控制器驱动；
- `/dev/ttyS9` 设备节点；
- GPIO3_B2/B3 引脚复用；
- IMX586、V4L2 和 ISP 驱动。

应用层负责：

```text
UART 收包
  -> 帧头、长度、序号、CRC 校验
  -> 读取 camera_id
  -> 命令分发
      -> camera 0 控制适配器 -> IQ 程序 0
      -> camera 1 控制适配器 -> IQ 程序 1
  -> 返回 ACK、状态或错误码
```

因此 UART 服务不应放在 `external/camera_engine_rkaiq/` 内。建议后续继续放在独立应用目录，例如 `app/camera_uart/`，把两套 IQ 程序当作两个下游控制端。现在只有一套 IQ 程序时，可以先让两个 `camera_id` 进入同一个临时适配器；以后两个 IQ 程序交付后，只替换分发适配器，不改 UART 驱动和串口协议。

UART 应用也不应直接通过 I2C 裸写 IMX586 曝光寄存器。曝光、增益、白平衡等需要通过对应 IQ/RKAIQ 控制接口完成，否则 IQ 状态、Sensor 驱动状态和实际寄存器可能不一致。

## 8. 推荐的后续实现顺序

1. 接第二个 3.3 V USB-TTL，完成第 6 节外部 echo 测试。
2. 连续运行 echo 压力测试，至少传输 1 MiB 随机数据，检查字节数和 SHA-256。
3. 定义二进制协议：magic、version、sequence、camera_id、command、payload length、payload、CRC16。
4. 先实现与相机无关的 `PING`、`GET_VERSION`、`GET_STATUS` 和错误 ACK。
5. 加入 camera 0/1 的路由，但暂时接现有的一套 IQ 程序。
6. 加入曝光、增益、白平衡、帧率等控制适配器，并逐项读回验证。
7. 两个独立 IQ 程序到位后，分别固定绑定两路 Sensor/ISP/IQ 文件，再替换两个适配器。
8. 最后做 UART 命令与双路 2～4 Hz 采集并发、异常命令、断线恢复和 30～60 分钟稳定性测试。

## 9. 备份、镜像与回滚

修改前完整 64 MiB boot 分区已备份：

```text
板端：/userdata/boot-partition-before-uart9-20260720.img
主机：backups/uart9_before_20260720/boot-partition-before-uart9-20260720.img
SHA-256：651b1cb63c0ea0e2603fd95e392156a8e305895bac168b8cede4f3a1f2241f94
```

基于板端原始 kernel/resource、只替换 UART9 DTB 的校正镜像：

```text
output/firmware/boot-uart9-board-base.img
大小：46459392 字节
SHA-256：ec59d2448f224203bbd0d10475f8bff077c57a699e45940d4072cf1d02f649d2
```

截至本文完成时，目标板正在运行首次写入的 UART9 镜像，`ttyS9` 和内部回环均已通过；上面的 `boot-uart9-board-base.img` 是随后按板端原始组件制作的校正版，目前只保存在主机，尚未二次写入目标板。

如需回滚，可在板端确认备份哈希后写回完整 boot 分区：

```sh
sha256sum /userdata/boot-partition-before-uart9-20260720.img
dd if=/userdata/boot-partition-before-uart9-20260720.img \
  of=/dev/mmcblk0p3 bs=4M conv=fsync
sync
reboot
```

写 boot 分区前必须再次确认目标确实是 `/dev/mmcblk0p3`，不能把命令用于其他板卡或不同分区布局。

## 10. 当前验收边界

本次可以确认：

- RK3576 的 UART9 已启用；
- `/dev/ttyS9` 和 HOT_SHOE1 的 M1 pinmux 对应正确；
- 应用能稳定配置 115200 8N1；
- 内核 UART 发送和接收链路可用；
- 无外部短接的控制器内部回环通过；
- 独立应用层 UART 测试工具已实现并在目标板编译、执行通过。

本次不能确认：

- HOT_SHOE1 引脚到电脑 USB-TTL 的外部电气双向通信；
- UART 命令对两路 IMX586 曝光、增益等参数的实际控制；
- 两个独立 IQ 程序的 camera 0/1 隔离。

后三项需要第二个 USB-TTL、正式控制协议和两套 IQ 程序到位后继续测试。
