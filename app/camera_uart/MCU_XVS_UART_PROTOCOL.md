# RK3576 通过 UART 控制 MCU 产生双相机 XVS

## 1. 方案边界

RK3576 不直接产生 XVS。`camera_aiq_test` 通过 UART 向 MCU 发送命令，MCU
使用硬件定时器产生稳定的 XVS，送到 `FSYNC_CAM`。板上的 U9803 再将它缓冲
为 1.8 V `PPS_OUT`，同时送到两颗 IMX586 的 pin 26。

原理图第 12 页中的 USB、UART 和 `FSYNC_CAM` 只是共用 CN4 连接器，彼此是
独立网络。USB 数据线不能直接变成 `FSYNC_CAM`。USB 仅可用于承载一个
USB-UART 设备；真正的 XVS 仍需 MCU 的定时器输出脚接 CN4 pin 23。

## 2. 接线

| RK3576/CN4 | MCU | 说明 |
| --- | --- | --- |
| pin 19 `UART_CAM1_TX` | MCU UART RX | RK3576 发命令 |
| pin 17 `UART_CAM1_RX` | MCU UART TX | MCU 返回 ACK/状态 |
| pin 23 `FSYNC_CAM` | MCU 定时器输出 | 空闲高、低脉冲有效 |
| GND | MCU GND | 必须共地 |

`UART9_RX/TX` 所在 VCCIO5 是 1.8 V。若 MCU IO 是 3.3 V，UART TX/RX 必须
经过双向电平转换。XVS 也按 1.8 V CMOS 设计；未核实 U9803 输入是否允许
3.3 V 之前，不要把 3.3 V 直接送入 `FSYNC_CAM`。

MCU 上电后必须立即把 XVS 配为推挽高电平。板上 `FSYNC_CAM` 有下拉，MCU
高阻时不是安全的空闲状态。

## 3. UART 参数和帧格式

- 115200 baud、8 数据位、无校验、1 停止位、无流控。
- ASCII，一帧以 `\r\n` 结束。
- CRC 为 CRC16-CCITT-FALSE：初值 `0xffff`，多项式 `0x1021`，不反射，
  XOROUT 为 `0x0000`。
- CRC 范围是不含 `$`、`*CRC` 和换行的正文。

请求格式：

```text
$XVS,<seq>,<command>[,<arg>...]*<CRC16>\r\n
```

成功应答：

```text
$ACK,<seq>,<command>[,<field>...]*<CRC16>\r\n
```

失败应答：

```text
$NACK,<seq>,<command>,ERROR=<reason>*<CRC16>\r\n
```

命令：

| 请求正文 | MCU 行为 | 成功应答命令 |
| --- | --- | --- |
| `XVS,seq,PING` | 通信检查 | `PONG` |
| `XVS,seq,IDLE` | 停定时器并主动输出高 | `IDLE` |
| `XVS,seq,START,4000,10` | 连续 4 Hz、低 10 us | `START` |
| `XVS,seq,START,2000,10` | 连续 2 Hz、低 10 us | `START` |
| `XVS,seq,COUNT,4000,10,1000` | 精确输出 1000 个脉冲 | `COUNT` |
| `XVS,seq,STOP` | 停定时器并主动输出高 | `STOP` |
| `XVS,seq,STATUS` | 查询 MCU 实际状态 | `STATUS` |

`START` 和 `COUNT` 中的频率单位是 mHz，不是 Hz。状态应答字段必须完整：

```text
ACK,seq,STATUS,STATE=IDLE,FREQ_MHZ=4000,LOW_US=10,
PULSE_COUNT=1000,LAST_TRIGGER_ID=1000
```

`PULSE_COUNT` 是 MCU 上电后的累计实际输出脉冲数，`LAST_TRIGGER_ID` 每个
已输出下降沿加一。ACK 表示命令已经被 MCU 接受；定量输出是否完成要查询
`STATE`，完成后必须回到 `IDLE` 并保持高电平。

## 4. MCU 固件要求

不要用 UART 中断里的延时或主循环翻转 GPIO。使用硬件定时器/PWM：

- 4 Hz：周期 250000 us，低电平 10 us，高电平 249990 us。
- 2 Hz：周期 500000 us，低电平 10 us，高电平 499990 us。
- 下降沿定义为一个新的 `trigger_id`。
- `COUNT` 用定时器更新中断计数，在最后一个脉冲结束后关闭定时器并拉高。
- `STOP`、`IDLE`、复位和错误恢复都必须关闭定时器并主动拉高。
- CRC 错误、非法频率、非法脉宽不得改变当前输出，返回 NACK。

只有给出 MCU 的具体型号、时钟和 UART/定时器引脚后，才能把这一协议落成
可编译的 MCU 工程。RK3576 侧不依赖具体 MCU 型号。

## 5. RK3576 使用

先确认没有其他程序占用 ttyS9，例如 `camera_uartd`。启动同一个程序：

```sh
./camera_aiq_test --sync-uart /dev/ttyS9
```

启动时程序会依次执行 `PING` 和 `IDLE`。任何一步超时、CRC 错误或收到
NACK，程序都会拒绝进入控制台，避免在 MCU 状态未知时开始采集。

交互命令：

```text
sync-controller-status
stream-start all
sync-start 4
sync-status
sync-stop
stream-stop all
```

定量 1000 帧验证时，IMX586 Slave 模式的第一个 XVS 通常用于 pre-shutter。
因此先单独发送一个预备脉冲，再记录基线，再发送 1000 个测试脉冲：

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

4 Hz 的 1000 个脉冲约需 250 秒；2 Hz 约需 500 秒。软件验收应看到 MCU
累计脉冲准确增加、两路各增加 1000 帧、两路帧数差为 0、
`sequence_drops=0`，停止后帧数不再增长。`sync-status` 中的 `delta_ns` 仍是
ISP/V4L2 缓冲时间差，只能辅助诊断。最终曝光起点误差仍需示波器同时观察
`FSYNC_CAM`、`PPS_OUT`、cam0 pin26 和 cam1 pin26。

## 6. 本机无 MCU 测试

```sh
make -f Makefile.camera_aiq check-xvs-uart
```

该测试使用伪终端模拟 MCU，覆盖 115200 串口打开、CRC、序号、PING、IDLE、
START、COUNT、STOP 和 STATUS。它证明软件协议链路，不证明硬件电平或脉冲
质量。
