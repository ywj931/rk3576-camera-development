# RV1126B xdas 相机功能迁移状态（2026-08-26）

## 结论

本次目标板已确认是 `root@10.100.2.99`：

```text
model=Alientek RV1126B Board
compatible=rockchip,rv1126b-evb4-v10,rockchip,rv1126b
arch=aarch64
os=Buildroot 2024.02
kernel=6.1.141
```

xdas 串口协议、命令分发、保存状态机、Rockchip MPP JPEG、EXIF 和 CSV 已移植，
并在这台 RV1126B 上运行通过。板上当前只有一路真实 IMX586 完成 media graph：
`/dev/video23` 输出 4000x3000 NM12；第二路 `/dev/video31` 是无远端 sensor 的
800x600 ISP 虚拟链路。因此当前可以确认“单路 IMX586 保存功能通过”和“双实例保存
后端通过”，不能确认“双相机实拍通过”。

本次没有修改 ISP、IQ 文件、DTS 或 sensor 驱动，这些仍由小马负责。

## 功能状态

| 功能 | 代码状态 | RV1126B 实测结论 | 尚缺验收 |
|---|---|---|---|
| xdas UART 115200 8N1 | 已实现 | 板端 PTY 拆包、粘包、重复帧、噪声、坏 CRC 恢复通过 | 外部控制器、物理接线和长线稳定性 |
| `00 00` 查询版本 | 已实现 | 参考帧逐字节通过 | 产品版本号定版 |
| `00 02` 查询模式 | 已实现，当前固定 UVC | 参考帧逐字节通过 | 与 USB Gadget 实际状态联动 |
| `00 03` 查询容量 | 已实现 | 编码和异常映射测试通过 | 用产品最终保存挂载点做容量/满盘测试 |
| `00 04` 查询时间 | 已实现 | 17 字节微秒格式测试通过 | PPS/PTP 锁定状态另行验收 |
| `01 11/12` 本机保存 | 已实现 | 单路真实 IMX586 输入、幂等、停止排空、失败粘滞通过 | 外部 UART 发命令及连续长测 |
| JPEG/EXIF/CSV | 已实现 | 板载 MPP 15bf88a 编码和 `ffprobe` 解码通过 | 真实 PPS/trigger 元数据闭环 |
| 双路保存状态机 | 已实现 | 双实例启动、回滚和停止通过 | 第二路真实 sensor 接通后的双摄实拍 |
| `01 15` UVC | 已接现有 UVC 后端 | 命令和状态映射测试通过 | RV1126B Gadget 枚举、FPS、断开重连 |
| `01 16` 设置时间 | 已实现 | 解析、回调、ACK 测试通过 | 未主动改板端系统时间，需联机窗口验收 |
| `01 14` UDISK | 未实现，返回 `FF` | 不伪造成功 | 卸载、只读导出、断电恢复状态机 |
| `01 17-1A` 曝光/PPS/快门/ISO | 未接入，返回 `FF` | 不伪造成功 | 等 ISP/IQ 和同步接口稳定后接 adapter |

## RV1126B 实测证据

完整 `camera_aiq_test` 已使用板载 `librkaiq.so` 和
`librockchip_mpp.so.1` 的头文件/ABI 完成交叉链接，部署后二进制依赖全部解析。
以下四项在板端通过：

```text
XDAS_PROTOCOL_SELF_TEST_OK
PHOTO_EXIF_SELF_TEST_OK
SYNC_BIND_SELF_TEST_OK
CONTROL_UART_PROTOCOL_SELF_TEST_OK
```

独立协议程序在板端通过：

```text
XDAS_PROTOCOL_TEST_OK
crc=CRC8_D5
reference=version,mode,capacity,get_time,set_time,uvc
save=global,targeted,idempotent,sticky_error,rollback
pty=fragmented,repeated,crc_recovery,resync
```

实时抓帧时 `/dev/video23` 的格式为 4000x3000 NM12、两平面，单帧共
18,000,000 字节。2026-08-26 重新现抓一帧后，使用 xdas 生产命令分发逻辑执行：

```text
SAVE_ON -> 重复 SAVE_ON -> 提交真实 IMX586 帧
-> SAVE_OFF 等待编码/CSV 队列排空 -> 重复 SAVE_OFF
```

单路模式的结果为：只创建 `cam0`，生成一张 4000x3000 JPEG 和一份严格两行的
CSV；没有创建 `cam1`。`ffprobe` 结果为：

```text
mjpeg,4000,3000,yuvj420p
```

同一程序还以双实例模式完成 640x480 合成帧回归，并以同一张真实 IMX586 帧验证
两个 MPP/EXIF/CSV 后端实例。该测试用于验证双路软件状态机，不代表两路 sensor
同时取帧。

错误路径也已覆盖：非法 metadata 会使首次和重试 `SAVE_OFF` 都返回 `error=02`；
新 `SAVE_ON` 才清除粘滞会话错误。双路启动任一路失败会回滚已启动的一路。保存停止
会等待编码和 CSV 队列排空，并对 CSV 执行 `fflush`/`fsync`；编码、EXIF、CSV、写盘、
非法 metadata 和队列溢出不再返回伪成功 ACK。

## 当前工程边界

1. 完整主程序仍按 RK3576 产品形态固定创建两个 RKAIQ context，默认 IQ 目录也是
   `/etc/iqfiles/cam0`、`/etc/iqfiles/cam1`。当前 RV1126B 只有 phy0 的 IMX586，
   所以不能把完整双路 daemon 标记为可部署。应等第二路 ISP/sensor 完成后再接，不应
   在本任务中伪造第二路 AIQ。
2. `/dev/ttyS4` 和 `/dev/ttyS5` 均为可访问的 16550A UART，当前没有进程占用，
   `tx=0 rx=0`。本次只做了读取 termios 的无发送检查；没有外部相机控制器或物理回环，
   因而不能声称板级 UART RX/TX 已通过。
3. 实时 IMX586 图像是真实帧，但集成测试中的 trigger、UTC、曝光和 ISO metadata 是
   确定性测试值。真实 PPS/XVS/trigger 到帧绑定仍需要 MCU 接线后验收。
4. UVC、UDISK、外部设时和曝光类命令不是本轮“串口控制本机保存”的通过项。

## 下一步验收

1. 明确 xdas 控制口接 `/dev/ttyS4` 还是 `/dev/ttyS5`，接上位机或 USB-UART 后逐条
   发送参考帧，并同时核对 ACK、文件数、JPEG 解码、EXIF 和 CSV。
2. 第二路 sensor 接通后，记录 phy1 sensor 名称、IQ 目录、params/mainpath 节点，
   再运行完整双路 daemon 和无 payload 的全局 `SAVE_ON/OFF`。
3. 补 30-60 分钟连续保存、磁盘满、掉电恢复、坏 CRC 风暴和串口断线重连测试。
4. UVC/UDISK、曝光、PPS、快门、ISO 按独立功能逐项接 adapter，不与当前保存通过项
   混写为已完成。
