# 双摄 HTTP MJPEG 输出

网络输出已经改为 HTTP MJPEG，不再依赖 RTSP 库。程序使用 RKMPP 将两路
4000x3000 NV12 分别编码成 JPEG，并由同一个轻量 HTTP 服务发布：

```text
http://<board-ip>:8080/cam0
http://<board-ip>:8080/cam1
http://<board-ip>:8080/
```

`/cam0` 和 `/cam1` 是 `multipart/x-mixed-replace` MJPEG 流，可以直接放入浏览器
地址栏，也可以用网页 `<img>`、VLC 或 ffmpeg 打开。根路径 `/` 同时显示两路。

## 手动常驻运行

```sh
systemctl stop rkaiq_3A.service
cd /root/camera_uart
./camera_aiq_test --daemon
```

`--daemon` 会自动启动 cam0/cam1 采集及两路 HTTP 输出，不读取测试命令。收到
SIGTERM 或 Ctrl+C 后会依次停止采集、编码和 HTTP 服务。

## 开机自动启动

将程序和仓库中的 `camera-http.service` 放到板卡后执行：

```sh
install -m 0755 camera_aiq_test /root/camera_uart/camera_aiq_test
install -m 0644 camera-http.service /etc/systemd/system/camera-http.service
systemctl daemon-reload
systemctl enable --now camera-http.service
```

服务与 `rkaiq_3A.service` 冲突，避免两个进程同时持有 RKAIQ。状态和日志：

```sh
systemctl status camera-http.service
journalctl -u camera-http.service -f
```

## 带宽说明

MJPEG 的兼容性高、浏览器能直接显示，但同等画质下带宽明显高于 H.264/RTSP。
当前默认是每路 4000x3000、10 fps、JPEG quality 75。用于公网或低带宽链路时，
应降低分辨率、帧率或 JPEG quality，或者恢复 H.264 并使用支持浏览器播放的封装层。

## RK1126 移植边界

HTTP socket 和 MJPEG multipart 层只依赖 Linux/POSIX，可以原样复用。MPP JPEG
编码层使用 Rockchip MPP API，也可以复用到提供相同 API 的 RK1126 SDK。需要按
RK1126 实际 SDK 重新适配的是 RKAIQ 初始化、media graph、V4L2 节点和工具链库
路径；不要把 RK3576 的 `/dev/video22`、`/dev/video31` 节点号直接写到新板上。
