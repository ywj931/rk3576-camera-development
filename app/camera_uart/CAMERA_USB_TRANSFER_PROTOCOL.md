# RV1126B 单路照片可靠传输协议 V1

## 目标

RV1126B 通过 USB 3.0 NCM 虚拟网口上的 TCP 长连接，把单路 IMX586 照片发送给
RK3588。V1 只解决整张照片的边界、身份、完整性和明确 ACK，不实现分块或断点续传。

NCM 只提供网络接口；本协议同样可以先在普通以太网上测试。切换到 NCM 时只修改
接收端 IP，不修改照片协议。

## TCP 消息

每张照片是一条连续 TCP 消息：

```text
80-byte PhotoHeader | data_length bytes photo data | 24-byte ACK
```

所有多字节整数使用网络字节序（big-endian）。TCP 是字节流，接收端必须按固定头长度
和 `data_length` 精确读取，不能假设一次 `recv()` 对应一张照片。

### PhotoHeader

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `DIMG` |
| 4 | 1 | version | 当前为 `1` |
| 5 | 1 | header_size | 固定 `80` |
| 6 | 1 | format | `1=JPEG, 2=PNG, 3=NV12, 4=DNG_RAW10` |
| 7 | 1 | camera_id | V1 允许 `0` 或 `1`；当前 RV1126B 只发送 `0` |
| 8 | 8 | session_id | 程序启动时生成，防止重启后 frame_id 重复 |
| 16 | 8 | frame_id | 相机帧编号 |
| 24 | 8 | exposure_start_utc_ns | 曝光开始 UTC Unix 纳秒 |
| 32 | 8 | exposure_duration_ns | 曝光时长纳秒；结束时间由两者相加得到 |
| 40 | 8 | data_length | 照片字节数，V1 上限 1 GiB |
| 48 | 32 | SHA-256 | 只覆盖随后 `data_length` 字节的照片数据 |

`trigger_id` 不属于V1传输身份，继续保存在JPEG EXIF/CSV中。时间字段必须明确其来源；
没有真实曝光边沿时，不能把 Trigger 加固定偏移伪称为热靴实测时间。

### ACK

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `DACK` |
| 4 | 1 | version | 当前为 `1` |
| 5 | 1 | ack_size | 固定 `24` |
| 6 | 1 | status | 见下表 |
| 7 | 1 | camera_id | 与请求一致 |
| 8 | 8 | session_id | 与请求一致 |
| 16 | 8 | frame_id | 与请求一致 |

状态码：

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | `OK` | 长度、SHA-256和文件写入均成功 |
| 1 | `BAD_HEADER` | 固定头非法 |
| 2 | `BAD_LENGTH` | 未收到声明长度或落盘长度不一致 |
| 3 | `SHA256_MISMATCH` | 接收文件指纹与发送端不一致 |
| 4 | `WRITE_ERROR` | 创建、写入或改名失败 |
| 5 | `UNSUPPORTED_FORMAT` | 不支持的输出格式 |

发送端只在 ACK 的 `session_id/camera_id/frame_id` 完全匹配且状态为 `OK` 时，把照片
标记为已送达。超时、断线和 NACK 都保留本地文件，V1 直接整张重传。

## 接收落盘

接收端先写 `<final-name>.part`，完整收到 `data_length` 字节后重新计算文件 SHA-256。
长度和 SHA-256 都一致才原子改名为正式文件，然后发送 ACK。这样断线留下的半张照片
不会被误认为完整照片。

默认 ACK 表示“完整文件已写入并校验”，不承诺突然断电后仍然存在。要求持久化 ACK
时，接收端增加 `--durable`，在改名前执行 `fsync()`；连续高速拍摄时需要实测该选项
对磁盘吞吐的影响。

TCP 已负责链路丢包、乱序和重传，V1 使用 SHA-256 后不再增加 CRC32。

## 构建和运行

Ubuntu/RK3588 接收端：

```sh
make -f Makefile.camera_aiq camera_transfer_receiver
./camera_transfer_receiver /data/camera_rx 46000 0.0.0.0
```

发送一张照片：

```sh
./camera_transfer_sender 192.168.88.1 46000 PHOTO.jpg \
  0 100 1710000000123461789 5000000 jpeg
```

输出 `PHOTO_TRANSFER_OK ... ack=OK` 才表示接收端已经完成长度和 SHA-256 校验。

## 2026-08-27 RV1126B 实测

RV1126B `/dev/video23` 现抓一帧 4000x3000 NM12，板载 MPP 编码出 1,937,492 字节
JPEG。板端 aarch64 发送程序通过 TCP 把它发送到另一台 Linux 主机：

```text
camera_id=0
frame_id=100
sender_sha256=97fd3b2a87e46c14df96c639ca2b3afc1c1a57616f665e4f84c493e624032511
receiver_sha256=97fd3b2a87e46c14df96c639ca2b3afc1c1a57616f665e4f84c493e624032511
ack=OK
ffprobe=mjpeg,4000,3000,yuvj420p
```

该轮使用板上现有 `eth0` 验证应用协议。当前板端内核未启用
`CONFIG_USB_CONFIGFS_NCM/ECM/RNDIS`，没有对应模块或 `usb0`；UDC 当前协商为
USB 2.0 High-Speed 且状态为 `addressed`。在重新构建匹配内核并确认物理 USB 3.0
枚举前，不能把上述结果标记为 USB NCM 实链通过。

该协议默认用于点对点 USB 网络，没有加密和身份认证。如果以后允许从普通局域网访问，
应增加 TLS 或限制监听地址和防火墙规则。
