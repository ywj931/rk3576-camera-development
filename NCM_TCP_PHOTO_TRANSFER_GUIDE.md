# RV1126B NCM + TCP 照片传输说明

本文对应 `camera_uart` 当前实现，供 RV1126B 与 RK3588 联调和交付使用。
协议实现以源码为准；字段和错误码不要由接收端自行推测。

## 1. 分层和方向

```text
RV1126B USB device
  usb0 = 192.168.77.2/24
        │ USB CDC-NCM
RK3588 USB host
  usb1（或 enx*）= 192.168.77.1/24
        │ TCP/IPv4 : 46000
camera_aiq_test / stitch_transfer_worker  ─────>  camera_transfer_receiver
```

NCM 只负责把 USB 线变成 IP 网卡，不定义照片格式。照片格式由本仓库的 TCP 协议
定义；因此可以先在普通以太网上测试同一协议，换成 NCM 时只需要把目标地址改为
`192.168.77.1`。

控制串口和照片 TCP 是两条独立链路：默认配置中 XDAS 控制串口为 `/dev/ttyS1`，
GNSS/PPS 串口为 `/dev/ttyS2`，照片通过 NCM 网卡走 TCP `46000` 端口。

## 2. RV1126B 应用入口

双路模式下，两个 `4000x3000` NV12 帧先在 RV1126B 合成为一张竖向
`4000x6000` JPEG。JPEG 写入本地 spool 后才进入传输队列：

```text
/userdata/camera/stitch/
  stitch_pair_<pair>_top_<cam0-frame>_bottom_<cam1-frame>_<timestamp>.jpg
  stitch_metadata_v2.csv
```

传输启动方式有两种：

1. 守护进程启动时传入 `--xdas-stitch-host 192.168.77.1 --xdas-stitch-port 46000`，
   然后通过 XDAS 的全局 `save-on` 命令启动合成和传输。
2. 在 `camera_aiq_test` 控制台中执行：

   ```text
   stitch-transfer-start /userdata/camera/stitch 192.168.77.1 46000
   ```

   该命令要求 cam0、cam1 两路采集都已经运行。

当前固件的配置入口是 `/etc/pcl-camera.conf`，关键项如下：

```sh
PCL_CAMERA_COUNT=2
PCL_CAMERA_XDAS_STITCH_HOST=192.168.77.1
PCL_CAMERA_XDAS_STITCH_PORT=46000
PCL_CAMERA_SAVE_ROOT=/userdata/camera
```

`PCL_CAMERA_AUTOSTART` 默认是 `0`。设置为 `1` 后，`S60pcl-camera start` 才会在
后台启动 `pcl-camera-service`；否则需要由上层显式启动程序。

## 3. TCP 消息格式

每张照片在同一条 TCP 连接上按下面顺序发送：

```text
80-byte PhotoHeader | data_length-byte JPEG payload | 24-byte ACK
```

TCP 是字节流。接收端必须循环读取到指定字节数，不能假设一次 `recv()` 就是一整帧。
所有多字节整数均为网络字节序（big-endian）。当前版本没有额外 CRC；照片完整性由
SHA-256 校验。

### PhotoHeader（固定 80 字节）

| 偏移 | 长度 | 字段 | 当前含义 |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `DIMG` |
| 4 | 1 | `version` | `1` |
| 5 | 1 | `header_size` | `80` |
| 6 | 1 | `format` | `1=JPEG`；协议还保留 `2=PNG`、`3=NV12`、`4=DNG` |
| 7 | 1 | `camera_id` | `0` 或 `1`；合成照片当前固定发送 `0` |
| 8 | 8 | `session_id` | 传输启动时随机生成，防止重启后身份冲突 |
| 16 | 8 | `frame_id` | 合成照片使用 `pair_id` |
| 24 | 8 | `exposure_start_utc_ns` | 曝光开始 Unix 时间，纳秒 |
| 32 | 8 | `exposure_duration_ns` | 曝光时长，纳秒 |
| 40 | 8 | `data_length` | 后续照片字节数，必须大于 0 且不超过 1 GiB |
| 48 | 32 | `sha256` | 仅覆盖后续照片 payload 的 SHA-256 |

合成照片的 `camera_id=0` 是“合成传输身份”，不是表示只来自物理 cam0；两路物理
相机、触发号和帧号保存在 JPEG EXIF 及 `stitch_metadata_v2.csv` 中。

### ACK（固定 24 字节）

接收端完整落盘并校验后返回：

| 偏移 | 长度 | 字段 | 当前含义 |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `DACK` |
| 4 | 1 | `version` | `1` |
| 5 | 1 | `ack_size` | `24` |
| 6 | 1 | `status` | 见下表 |
| 7 | 1 | `camera_id` | 必须与请求一致 |
| 8 | 8 | `session_id` | 必须与请求一致 |
| 16 | 8 | `frame_id` | 必须与请求一致 |

状态码：

| 值 | 名称 | 含义 |
| ---: | --- | --- |
| 0 | `OK` | 长度、SHA-256 和文件写入均成功 |
| 1 | `BAD_HEADER` | 头部非法 |
| 2 | `BAD_LENGTH` | payload 长度不足或不一致 |
| 3 | `SHA256_MISMATCH` | 接收内容指纹不一致 |
| 4 | `WRITE_ERROR` | 创建、写入、改名或持久化失败 |
| 5 | `UNSUPPORTED_FORMAT` | 接收端不支持该格式 |

发送端只有在 ACK 的 `status=OK` 且 `session_id`、`camera_id`、`frame_id` 全部匹配
时，才把本地照片标记为送达。

## 4. 接收端行为

接收端程序是 `camera_transfer_receiver`。它按以下顺序处理一张照片：

1. 精确读取 80 字节头并验证 `DIMG/version/header_size`。
2. 当前只接受 JPEG；精确读取 `data_length` 字节到临时 `.part` 文件。
3. 校验实际长度和 SHA-256。
4. 通过原子硬链接发布正式文件和 80 字节 `.meta` 侧车文件。
5. 返回 24 字节 `DACK`，然后继续处理同一 TCP 连接上的下一张照片。

默认正式文件名为：

```text
session_<session_id-16进制>_cam<camera_id>_frame_<frame_id-20位>.jpg
```

`--durable` 会在 ACK 前对文件和目录执行 `fsync()`；不加该选项时，ACK 表示已经
完成用户态写入和 SHA-256 校验，不代表突然掉电后的持久化保证。

## 5. 断线、重试和恢复

`stitch_transfer_worker` 为串行发送队列。连接失败、超时、NACK 或 ACK 身份不匹配时：

- 不删除 RV1126B 本地 JPEG；
- 断开并重新连接；
- 最多按默认策略重试，退避约 `250 ms` 到 `5 s`；
- 下次启动时扫描 `stitch_metadata.csv`、`stitch_metadata_v2.csv` 和孤立的
  `stitch_pair_*.jpg`，恢复未完成的队列。

只有收到匹配的 `DACK/OK` 后才删除本地 JPEG。接收端如果只是 `recv()` 后写文件、
不解析头部、不校验 SHA-256、不返回 24 字节 DACK，发送端会持续认为失败，旧照片会
不断重传。

运行中的 `camera_aiq_test` 控制台可执行：

```text
stitch-transfer-status
stitch-photo-status
```

重点检查：`enqueued`、`delivered`、`retries`、`failed`、`backlog`、`last_error`，
以及 `paired`、`saved`、`write_errors`。网络有流量不等于已经送达，必须同时看到
接收端 `PHOTO_RECEIVED` 和发送端 `delivered` 增长。

## 6. 构建和启动

在有 OpenSSL `libcrypto` 的 Linux 主机上构建接收端、发送端和协议自测：

```sh
cd /path/to/camera_uart
make -f Makefile.camera_aiq camera_transfer_receiver
make -f Makefile.camera_aiq camera_transfer_sender
make -f Makefile.camera_aiq check-camera-transfer
make -f Makefile.camera_aiq check-stitch-transfer
```

RK3588 接收端示例：

```sh
ip address replace 192.168.77.1/24 dev usb1
ip link set usb1 up
./camera_transfer_receiver /data/camera_rx 46000 192.168.77.1 --durable
```

如果 RK3588 的网卡名称不是 `usb1`，替换为实际的 NCM host 接口名。收到启动日志
`TRANSFER_RECEIVER_READY` 后再启动 RV1126B 的传输。

单张协议联调可使用：

```sh
./camera_transfer_sender 192.168.77.1 46000 PHOTO.jpg \
  0 100 1710000000123461789 5000000 jpeg
```

输出 `PHOTO_TRANSFER_OK ... ack=OK` 才算单张传输成功。

## 7. NCM 配置边界

本 Git 仓库保存的是应用和 TCP 协议源码；NCM gadget、USB 网卡地址和内核选项由
RV1126B SDK 的设备/内核 overlay 负责，不会由本目录的 `make` 自动安装。SDK 侧至少
需要：

```text
CONFIG_USB_CONFIGFS_NCM=y       # RV1126B gadget
CONFIG_USB_USBNET=y             # RK3588 host
CONFIG_USB_NET_CDC_NCM=y        # RK3588 host
```

RV1126B gadget 启动后应出现 `usb0=192.168.77.2/24`；RK3588 host 侧应出现 NCM
网卡并配置 `192.168.77.1/24`。先验证两端 IP、再验证 TCP `46000`、最后验证
`PHOTO_RECEIVED`/`DACK`，不要用单纯 `ping` 或裸 TCP 流量代替应用协议验收。

## 8. 交付清单

应用协议相关源码已经在本仓库 Git 中：

```text
camera_transfer_protocol.h/.cpp       编解码、SHA-256、错误码
camera_transfer_client.h/.cpp         RV1126B TCP 客户端
camera_transfer_receiver.cpp          RK3588 TCP 接收端
camera_transfer_sender.cpp             单张照片联调工具
stitch_transfer_worker.h/.cpp          合成照片队列、重试、断点恢复
camera_photo_backend.h/.cpp            双路合成、EXIF/CSV、入队
CAMERA_USB_TRANSFER_PROTOCOL.md        字段级协议参考
NCM_TCP_PHOTO_TRANSFER_GUIDE.md        本交付说明
```

Git 仓库无远程地址；交付时复制整个仓库或基于 `git log -1` 指定提交。NCM 的 SDK
overlay 和 RK3588 host 内核配置必须随板卡 SDK/镜像一并交付。
