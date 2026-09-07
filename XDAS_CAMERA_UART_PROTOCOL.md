# xdas 兼容相机控制串口

## 1. 作用范围

该接口让 RV1126B 相机主板作为 xdas 控制端眼中的“相机”，优先完成
本机 JPEG/EXIF 保存开关。协议层只依赖 C++17 和 Linux/POSIX，不依赖 RKAIQ、MPP
或固定 `/dev/videoN`，可以随应用层迁移到 RV1126B。

外部相机控制与内部 XVS/PPS MCU 是两个角色，推荐使用两路 UART：

- `--xdas-uart`：上位机到相机主板，使用本文件的 xdas 二进制协议。
- `--sync-uart`：相机主板到同步 MCU，继续使用 `$XVS/$EVT` 内部协议。

程序拒绝让两种协议打开同一个 UART，也拒绝同时启用 `--xdas-uart` 和旧的
`--uart/--control-uart` 相机控制入口。

## 2. 串口和帧格式

- 115200 baud、8N1、无流控。
- 请求：`AA | 总长度 | cmd0 | cmd1 | payload... | CRC8`。
- 响应：`55 | 总长度 | cmd0 | cmd1 | error | payload... | CRC8`。
- CRC-8：初值 `0x00`，多项式 `0xD5`，MSB-first；不包含末尾 CRC 字节。
- 当前请求最大 64 字节。解析器支持粘包、拆包、前导噪声和 CRC 错误恢复。

错误码沿用 xdas 定义：

| 错误码 | 含义 |
|---|---|
| `00` | 成功 |
| `01` | 请求头错误 |
| `02` | 命令参数、当前状态或后端执行失败 |
| `03` | 长度错误 |
| `04` | CRC 错误 |
| `FF` | 当前版本不支持该命令 |

当请求头、长度或 CRC 错误时，响应中的 `cmd0/cmd1` 固定为 `FF FF`，与
xdas V2.1 的协议错误格式一致。

## 3. 当前命令

| cmd0 cmd1 | 功能 | 状态 | 说明 |
|---|---|---|---|
| `00 00` | 获取版本 | 已实现 | 返回 byte/string 类型 `02` 和版本字符串 |
| `00 02` | 获取模式 | 已实现 | 当前返回 UVC 模式，格式与 xdas 示例一致 |
| `00 03` | 获取容量 | 已实现 | 返回保存分区剩余/总容量，单位 MB、小端 `uint32` |
| `00 04` | 获取时间 | 已实现 | 返回 Linux `CLOCK_REALTIME`，固定微秒格式；不代表 UTC 已锁定 |
| `01 11` | 开始本机保存 | 已实现 | 启用所有已配置相机的 JPEG+EXIF 保存 |
| `01 12` | 停止本机保存 | 已实现 | 停止所有已配置相机并等待已入队图片落盘 |
| `01 04` | 重启设备 | 已实现 | 校验空 payload 后先返回 ACK，再异步 `sync` + `reboot(RB_AUTOBOOT)` |
| `01 15` | 设置 UVC 模式 | 已实现 | 启动双路 UVC producer，USB Gadget 保持由系统配置 |
| `01 14` | 设置 UDISK | 未实现 | 没有安全的块设备导出和卸载状态机，返回 `FF` |
| `01 16` | 设置时间 | 已实现 | 严格解析 17 字节 Unix 时间，成功返回 `40 00`；保存中拒绝校时 |
| `01 17` | 设置最大曝光上限 | 已实现 | payload 为档位 `0..8`，映射 50000/20000/10000/5000/3333/2500/2000/1667/1250 us；双路先读取旧值，任一路失败则回滚已应用相机并返回 `error=02` |
| `01 18` | 设置 PPS 频率 | 未实现 | 当前没有与同步板的受控输出接口，返回 `FF` |
| `01 19` | 设置快门档位 | 已实现 | 一个 payload，`0` 自动；`1`-`21` 对应 16 s 至 1/16000 s，作用于全部已配置相机 |
| `01 1A` | 设置 ISO 档位 | 已实现 | 一个 payload，`0` 自动；`1`-`6` 对应 ISO 100/200/400/800/1600/3200，作用于全部已配置相机 |

xdas 原始无 payload 命令保持为全局命令。在双路产品配置中表示 cam0+cam1，
在 RV1126B 当前单路验收配置中只表示 cam0：

```text
开始双路保存：AA 05 01 11 73
成功响应：    55 06 01 11 00 A7

停止双路保存：AA 05 01 12 D9
成功响应：    55 06 01 12 00 BA

重启设备：    AA 05 01 04 0A
成功响应：    55 06 01 04 00 30
```

`01 11/01 12` 是协议定义的开始/结束本机图片保存，不是 UVC 视频开关；`01 15`
启动 UVC 图像输出并要求相机采集已运行。v1.2 没有“停止 UVC 输出”的独立命令；
`01 14` 的语义是切换 UDISK，不是关闭 UVC。当前系统没有安全的 UDISK Gadget
切换和块设备卸载状态机，因此 `01 14` 保持返回 `error=FF`，避免错误 ACK。控制台
中的 `uvc-stop` 只能本机调用，尚未被映射成一个未在 X-DAS v1.2 定义的新命令。

重启回调只能确认请求已成功排队；串口 ACK 发送后才执行 `sync` 和
`reboot(RB_AUTOBOOT)`。若进程无 `CAP_SYS_BOOT`，后续内核重启会失败并输出
`XDAS_REBOOT_FAILED`，但这发生在 ACK 之后，无法再向已完成的串口事务返回 NACK。

为一块主板上的双 sensor 增加了可选 `camera_id` payload，不改变 framing：

```text
开始指定相机保存：AA 06 01 11 <camera_id> <CRC8>
停止指定相机保存：AA 06 01 12 <camera_id> <CRC8>
设置指定相机 UVC：AA 06 01 15 <camera_id> <CRC8>
```

`camera_id` 必须小于运行配置的 `camera_count`。双路产品允许 `0` 或 `1`；当前
RV1126B 单路验收只允许 `0`。无 payload 表示所有已配置相机；其他长度或编号返回
`error=02`。

设置时间保持 xdas 原始格式，不增加 `camera_id`：

```text
AA 16 01 16 <ssssssssss.uuuuuu> CRC8
```

应用通过 `clock_settime(CLOCK_REALTIME)` 设置主板系统时钟，要求 root 或
`CAP_SYS_TIME`。任一路照片保存开启时返回 `error=02`，避免落盘过程中产生时间
倒跳。成功 ACK 只表示系统调用成功，不表示 PPS/PTP 已锁定；推荐在 UVC 和保存
开始前校时。

快门和 ISO 命令的 payload 是档位值，而不是 `camera_id`，因此始终同步应用到
当前配置的所有相机。非零档位分别设置 RKAIQ 的曝光时间和模拟增益。由于当前
RKAIQ 公共接口只有整套 AE `OP_AUTO/OP_MANUAL` 模式，没有独立的“仅自动快门”或
“仅自动 ISO”模式，`01 19 00` 与 `01 1A 00` 都切换整套 AE 为自动；这两个 AUTO
命令不会伪造互相独立的状态。帧格式和部分参考向量如下：

```text
快门 1/250 s： AA 06 01 19 0F 2B
快门 AUTO：    AA 06 01 19 00 56
ISO 3200：     AA 06 01 1A 06 CA
ISO AUTO：     AA 06 01 1A 00 4B
快门成功 ACK： 55 06 01 19 00 FF
ISO 成功 ACK： 55 06 01 1A 00 E2
```

快门档位的微秒映射为：`1=16000000`、`2=8000000`、`3=4000000`、
`4=2000000`、`5=1000000`、`6=500000`、`7=250000`、`8=125000`、
`9=66667`、`10=50000`、`11=41667`、`12=33333`、`13=16667`、
`14=8000`、`15=4000`、`16=2000`、`17=1000`、`18=500`、`19=250`、
`20=125`、`21=63`。分数秒无法以整数微秒精确表示的档位按最接近的微秒值传给
RKAIQ。

成功 ACK 仅表示全部已配置相机的 RKAIQ 设置接口已接受请求；实际传感器曝光受当前
IMX586 模式、帧率和 IQ 约束，应用应以随后 `status` 中相应的
`exposure_manual/iso_manual`、请求值和实际值确认。任一相机未开流、RKAIQ 拒绝参数
或档位超出范围时返回 `error=02`，不会伪造成功 ACK。双摄执行时若后一路失败，程序
仅对 `01 17` 最大曝光上限执行“预读 + 失败回滚”；回滚失败会输出
`XDAS_MAX_EXPOSURE_ROLLBACK_ERROR`，便于现场定位两路状态不一致。`01 19` 和
`01 1A` 会按配置顺序调用全部相机，任一路失败即返回 `error=02`，但不会声称已
恢复前一路状态。后端失败会输出带相机编号和错误码的 `CAMERA_AIQ_ERROR`。

## 4. 保存语义

`01 11` 复用 `camera_photo_backend`：

- cam0 保存到 `<save-root>/cam0`，cam1 保存到 `<save-root>/cam1`。
- 输出为 MPP JPEG，写入 EXIF，并追加 `stage6_metadata.csv`。
- 只保存已经与 trigger 绑定的帧；未收到真实或模拟 trigger 时不会生成照片。
- 开始/停止命令是幂等的，便于上位机在 ACK 丢失后重试。
- 双路开始时，若任一路未开流或初始化失败，会停止本次已启动的另一路并返回失败。
- 停止命令在保存队列排空后才返回 ACK；ACK 表示本次保存状态转换已经完成。
- 编码、EXIF、CSV、写盘或队列溢出错误在本次保存会话内保持，停止命令排空后返回
  `error=02`，并在重复停止时保持失败，直到下一次开始保存；不会把已经发生的数据
  丢失伪装成成功。

CRC 正确只说明串口帧有效。成功 ACK 还要求采集正在运行、保存目录可创建、MPP
编码器初始化成功。后续验收仍需检查保存状态、JPEG/EXIF、CSV 和实际文件数量。

## 5. 启动方式

当前 RV1126B 示例中，外部控制使用 `/dev/ttyS1`，GPRMC/PPS 使用 `/dev/ttyS2`：

```sh
./camera_aiq_test --capture-daemon \
  --xdas-uart /dev/ttyS1 \
  --xdas-save-root /data/camera \
  --gnss-uart /dev/ttyS2 \
  --gnss-baud 115200 \
  --pps-device /dev/pps0
```

`--xvs-autostart-hz` 属于独立的 MCU XVS 控制链路，只有接入并配置
`--sync-uart` 时才启用；它不应和 GNSS/PPS 输入串口混用。

无硬件时运行主机协议和伪串口测试：

```sh
make -f Makefile.camera_aiq check-xdas-uart
```

在带 Rockchip MPP 的 ARM 板上验证实际保存链路：

```sh
make -f Makefile.camera_aiq check-xdas-save-rockchip
```

也可以把一张 NV12/NM12 帧送入同一保存状态机。最后一个参数是有效相机数；以下命令
用于当前只有 cam0 接通的 RV1126B：

```sh
./xdas_camera_save_integration_test \
  /userdata imx586_4000x3000.nv12 4000 3000 1
```

该测试覆盖 xdas 已知 CRC 向量、精确保存/快门/ISO ACK、CRC 错误、双路/单路保存映射、
重复命令、双路失败回滚、设时参考向量，以及伪串口拆包和错误恢复。Rockchip
集成测试进一步覆盖 `SAVE_ON -> 双路 MPP JPEG/EXIF/CSV -> SAVE_OFF 排空`。在其他
Rockchip SoC 上通过仍不等于 RV1126B 的 UART 电平、MPP ABI、eMMC 和真实 trigger
链路已经验收。

2026-08-26 的 RV1126B 实测范围和未通过项见
`RV1126B_XDAS_MIGRATION_STATUS_20260826.md`。
