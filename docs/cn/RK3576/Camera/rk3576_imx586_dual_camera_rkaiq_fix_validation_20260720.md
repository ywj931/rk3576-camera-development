# RK3576 + 双 IMX586：RKAIQ 双路采集修复与验证报告

日期：2026-07-20  
平台：RK3576，ISP39，Ubuntu 24.04，aarch64  
摄像头：两路 IMX586，4000×3000，SRGGB10 输入，NV12 输出  
串口：`/dev/ttyUSB0`，板端 USB 网络：`192.168.55.1`

## 1. 结论

本次已完成的是双路 RKAIQ 崩溃修复，不是最终的两个独立 IQ 程序版本。

- 双路 IMX586 可以同时打开，两个 NV12 输出都能连续取得 1000 个有效缓冲。
- 原来在 frame-id 13 附近出现的 `rkaiq_3A_server` SIGSEGV 已消失；测试期间 3A 进程保持存活。
- 单路 4000×3000、30 fps 通过；双路 4000×3000、30 fps 可以运行，但内核 CSI2 报错并出现周期性序号跳变，因此不能宣称“30 fps 双路零丢帧”通过。
- 把两颗传感器的 `vertical_blanking` 临时设为 19980 后，双路前 5 帧稳定为 4.00 fps 且无跳号；随后 RKAIQ 将该值恢复为 64，说明量产版本必须通过 RKAIQ 帧率 API 固定帧率，不能只写 V4L2 VBLANK。
- 因此，本平台的双采集功能可行；项目要求的 2～4 Hz 应作为正式工作点，30 fps 全分辨率双路不能作为当前验收点。

## 2. 备份

### 2.1 修改前源码备份

目录：

`backups/rkaiq_dual_camera_20260720_before_fix/`

备份了：

- `external/camera_engine_rkaiq/rkaiq/hwi_c/aiq_CamHwBase.c`
- `external/camera_engine_rkaiq/rkaiq/hwi_c/aiq_sensorHw.c`
- `external/camera_engine_rkaiq/rkaiq_3A_server/rkaiq_3A_server.cpp`

修改前 SHA-256：

```text
58d5b03076426223fa18f6c0943732c9f64252248b605189a3981350bcf7a006  aiq_CamHwBase.c
dc129122d26b4d2fdedfd5511e210b9d3ea3dd9134db83d58e23a7fa62edba43  aiq_sensorHw.c
366ed405801cee814a97879ec5d744a7a28b87436818a169ee61ccb77d59c7be  rkaiq_3A_server.cpp
```

加入帧率实验前的 server 增量备份在：

`backups/rkaiq_dual_camera_20260720_before_fps/`

### 2.2 板端原始程序备份

目录：`/root/rkaiq_backup_20260720_before_dual_fix/`

```text
ee3e5ae6b9b74f4eacd8f56021056e11edb7608ebc475ce8852fe57435ae5a88  librkaiq.so
bd7b443fd92bf853c71a1816f2ad8ea5ebeef88601b1fe1a8039f7ec321178c0  rkaiq_3A_server
```

回滚命令（板端 root）：

```sh
systemctl stop rkaiq_3A.service
install -m 755 /root/rkaiq_backup_20260720_before_dual_fix/rkaiq_3A_server /usr/bin/rkaiq_3A_server
install -m 644 /root/rkaiq_backup_20260720_before_dual_fix/librkaiq.so /usr/lib/librkaiq.so
ldconfig
systemctl start rkaiq_3A.service
```

## 3. 原因

双路开启后，两路 SOF 的 frame-id 不完全同步。某一路在查询指定 frame-id 的曝光历史时，C 版本只保留较短的历史窗口；当精确 id 不存在时又直接返回 `NULL`。随后 ISP39 的 `_setIspExp()` 无条件解引用曝光指针，最终在 3A 线程中 SIGSEGV。

这不是“一个 AIQ 进程管理两路”本身错误。当前 server 会为每个 media 节点建立独立 `rk_aiq_sys_ctx_t`，同时开启多摄并发标志；崩溃来自曝光历史查找和空指针处理不完整。

## 4. 源码修改

### 4.1 传感器曝光历史

文件：`external/camera_engine_rkaiq/rkaiq/hwi_c/aiq_sensorHw.c`

- 两个 SOF 路径的 `_effecting_exp_map` 保留阈值从 5 调为 10，给双路 frame-id 抖动增加缓冲。
- `SensorHw_getEffectiveExpParams()` 查找不到精确 id 时，先取小于目标 id 的最新项。
- 如果 map 非空但所有项都比目标 id 新，则退回 map 中最新项；只有 map 真正为空才返回 `NULL`。
- 日志带上 camera id、搜索 id、map 大小和 key 范围，便于后续确认是否还有时序异常。

### 4.2 ISP39 空指针保护

文件：`external/camera_engine_rkaiq/rkaiq/hwi_c/aiq_CamHwBase.c:4760` 和 `:4848-4891`

- `_setIspExp()` 仍只接收有效曝光参数。
- `_setIspConfig()` 在拿不到曝光 metadata 时记录 warning 并跳过 ISP 曝光字段，不再把 `NULL` 传入 `_setIspExp()`。
- ISP 参数合并和队列回收继续执行，避免一帧 metadata 缺失升级为整个 3A 进程崩溃。

### 4.3 3A server 生命周期和尺寸

文件：`external/camera_engine_rkaiq/rkaiq_3A_server/rkaiq_3A_server.cpp`

- 默认 prepare 尺寸固定为 `4000×3000`，与 IMX586 当前 media 格式一致。
- `--width/-w`、`--height/-H`、`--help/-h` 参数可用，并校验正数。
- 检查 AIQ init、prepare、start 返回值；失败时清理 context 并停止当前 engine 线程。
- stop/deinit 增加空指针保护。

## 5. 编译和部署

构建目录：`out/rkaiq_dual_fix`

使用 RK3576/ISP39/aarch64 配置构建。由于当前交叉工具链的 `mmap64` 声明问题，C/C++ 配置补充：

```text
-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
```

构建目标：`rkaiq`、`rkaiq_3A_server`。

产物 SHA-256：

```text
c648906539e7395705cfddc11cbeab36f2f3e7643d7b2a0bce4d87ed5d9892bd  out/rkaiq_dual_fix/rkaiq/all_lib/MinSizeRel/librkaiq.so
db0490f13e77039cc9954840901dd9e5b1e62f961b4fd8e0daa4314dbcacd2c7  out/rkaiq_dual_fix/rkaiq_3A_server/rkaiq_3A_server
```

当前板端 `/usr/lib/librkaiq.so` 和 `/usr/bin/rkaiq_3A_server` 与上述产物哈希一致，`rkaiq_3A.service` 已恢复为 active。

## 6. 验证记录

### 6.1 单路

设备别名：`/dev/video-camera0 -> video22`，`/dev/video-camera1 -> video31`。

每路执行 100 个目标缓冲，另跳过 10 个预热缓冲：

```sh
v4l2-ctl -d /dev/video-camera0 --set-fmt-video=width=4000,height=3000,pixelformat=NV12 \
  --stream-mmap=4 --stream-skip=10 --stream-count=100 --verbose
```

两路均返回 0；每路得到 110 条 DQBUF（10 条预热 + 100 条目标），帧大小 18000000 字节，约 30.02 fps，序号连续，无 select timeout。

### 6.2 双路 30 fps，4 个 mmap buffer

两路同时请求 4000×3000 NV12、各 1000 个目标缓冲，3A 运行期间保持存活，两路命令均返回 0；没有重新出现曝光 map empty、SIGSEGV 或 select timeout。

但序号统计为：

| 输出 | 有效 DQBUF | 最后序号 | 跳变事件 | 丢失序号 |
|---|---:|---:|---:|---:|
| cam0 | 1010 | 1162 | 153 | 153 |
| cam1 | 1010 | 1318 | 309 | 309 |

这说明“能持续采集”已经通过，“双路 30 fps 零丢帧”没有通过。

### 6.3 双路 30 fps，8 个 mmap buffer + CPU 固定

cam0 绑定 CPU6，cam1 绑定 CPU7，每路 8 个 buffer、500 个目标缓冲。两路均返回 0，但仍有：cam0 丢 78 个序号，cam1 丢 155 个序号。增加用户态缓冲和固定 CPU 没有改善比例，故根因不是普通 `v4l2-ctl` 调度饥饿。

测试期间内核出现：

```text
MIPI_CSI2 ERR2:0xf0000
```

因此还存在 CSI/ISP 全分辨率双路吞吐或链路稳定性问题。

### 6.4 双路约 4 Hz

临时执行：

```sh
v4l2-ctl -d /dev/v4l-subdev4 --set-ctrl vertical_blanking=19980
v4l2-ctl -d /dev/v4l-subdev9 --set-ctrl vertical_blanking=19980
```

两路最初 5 帧时间间隔约 249.84 ms，即 4.00 fps，序号连续。第 6 帧起 RKAIQ 把两路 VBLANK 恢复为 64，帧率回到 30 fps。

结论是传感器和链路可以在 4 Hz 双路工作，但必须由 RKAIQ API 固定 AE 帧率，不能只依赖一次 V4L2 控件写入。

## 7. 后续正确实现方式

当前 SDK 已提供：

```c
frameRateInfo_t info = { .mode = OP_MANUAL, .fps = 4 };
rk_aiq_uapi2_setFrameRate(ctx, info);
```

应在每个独立 `rk_aiq_sys_ctx_t` 完成 prepare/start 后调用一次，或把它作为两个 IQ 程序各自的启动参数。对于两个独立 IQ 程序，必须分别绑定 media 节点、sensor entity 和 IQ 路径，不能简单复制两个未限定设备的 server。

建议顺序：

1. 先把当前修复版在 4 Hz 双路连续运行 30～60 分钟，统计序号、CSI 错误和进程存活。
2. 将 `rk_aiq_uapi2_setFrameRate()` 接入当前 server 或后续两个独立 IQ 程序，分别固定为 4 fps。
3. 再验证 UART 应用层命令：启动/停止、单帧请求、曝光/增益设置和错误返回；UART 不直接承担 ISP 驱动逻辑。
4. 最后再做两个独立 IQ 文件的隔离测试和整机长稳测试。

## 8. 回归命令

板端恢复服务后检查：

```sh
systemctl is-active rkaiq_3A.service
pgrep -a rkaiq_3A_server
sha256sum /usr/lib/librkaiq.so /usr/bin/rkaiq_3A_server
```

双路验收至少检查三项：

1. 两个 `v4l2-ctl` 返回码均为 0；
2. `cap dqbuf` 的序号是否连续；
3. 3A 进程是否持续存在、`dmesg` 是否新增 CSI2 错误。
