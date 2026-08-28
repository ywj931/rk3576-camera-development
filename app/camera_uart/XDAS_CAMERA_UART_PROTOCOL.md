# xdas 兼容相机控制串口

## 1. 作用范围

该接口让 RK3576/后续 RV1126B 相机主板作为 xdas 控制端眼中的“相机”，优先完成
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
| `01 15` | 设置 UVC 模式 | 已实现 | 启动双路 UVC producer，USB Gadget 保持由系统配置 |
| `01 14` | 设置 UDISK | 未实现 | 没有安全的块设备导出和卸载状态机，返回 `FF` |
| `01 16` | 设置时间 | 已实现 | 严格解析 17 字节 Unix 时间，成功返回 `40 00`；保存中拒绝校时 |
| `01 17`-`01 1A` | 曝光/PPS/快门/ISO | 未接入 | 等 RV1126B ISP/IQ 和同步接口确定后接入，当前返回 `FF` |

xdas 原始无 payload 命令保持为全局命令。在双路产品配置中表示 cam0+cam1，
在 RV1126B 当前单路验收配置中只表示 cam0：

```text
开始双路保存：AA 05 01 11 73
成功响应：    55 06 01 11 00 A7

停止双路保存：AA 05 01 12 D9
成功响应：    55 06 01 12 00 BA
```

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

示例中外部控制使用 `/dev/ttyS8`，同步 MCU 使用 `/dev/ttyS9`：

```sh
./camera_aiq_test --all-daemon \
  --xdas-uart /dev/ttyS8 \
  --xdas-save-root /data/camera \
  --sync-uart /dev/ttyS9 \
  --sync-timer-hz 1000000 \
  --xvs-autostart-hz 4
```

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

该测试覆盖 xdas 已知 CRC 向量、精确保存 ACK、CRC 错误、双路/单路保存映射、
重复命令、双路失败回滚、设时参考向量，以及伪串口拆包和错误恢复。Rockchip
集成测试进一步覆盖 `SAVE_ON -> 双路 MPP JPEG/EXIF/CSV -> SAVE_OFF 排空`。在其他
Rockchip SoC 上通过仍不等于 RV1126B 的 UART 电平、MPP ABI、eMMC 和真实 trigger
链路已经验收。

2026-08-26 的 RV1126B 实测范围和未通过项见
`RV1126B_XDAS_MIGRATION_STATUS_20260826.md`。
