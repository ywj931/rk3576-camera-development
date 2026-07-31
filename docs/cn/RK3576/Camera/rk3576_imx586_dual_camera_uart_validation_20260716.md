# RK3576 双 IMX586 与 UART 分阶段验证报告

## 1. 文档信息

- 测试日期：2026-07-16～2026-07-17（主机日期）
- 测试平台：泰山派 RK3576、Linux 6.1.99、Ubuntu 24.04.4
- 图像传感器：2 路 Sony IMX586
- 需求来源：`RK3576 MIPI 相机模组任务计划 v0.2`
- 当前策略：先用现有的一套 RKAIQ 服务和同一份 IQ 数据完成双路基础验证，后续再切换为两路独立 IQ 数据
- 串口条件：主机当前的 `/dev/ttyUSB0` 是 CH340 调试串口，实测调试波特率为 1500000；板端业务 UART 为 `/dev/ttyS6`

> 重要结论：双摄像头硬件链路能够并发采集，当前失败是 RKAIQ C 实现中的曝光历史/回退逻辑退化和 ISP39 空指针解引用共同造成的。干净复现时，两路 NV12 在 frame-id 13 查找曝光参数失败，`rkaiq_3A_server` 随后以 SIGSEGV 退出。此问题解决前，阶段 1 尚不能验收，也不适合开始 UART 相机业务控制或 30～60 分钟整机稳定性测试。

## 2. 本次测试结论

| 验证项 | 结果 | 结论 |
| --- | --- | --- |
| 主机 `/dev/ttyUSB0` 调试串口 | 通过 | CH340，可登录板端并执行全部测试；实际为 1500000 8N1，不是阶段 4 的 115200 业务 UART |
| 板端业务 UART `/dev/ttyS6` 配置 | 通过（仅软件配置） | 可配置为 115200 8N1，配置后已恢复原状态；未做物理收发和协议测试 |
| 两颗 IMX586 枚举 | 通过 | 两颗 Sensor、两条 MIPI/CIF 链路、两个 ISP 虚拟设备均存在 |
| cam0/cam1 设备映射 | 通过 | 当前 `/dev/video-camera0 -> /dev/video22`，`/dev/video-camera1 -> /dev/video31` |
| 单路 ISP/NV12 4000×3000 | 通过 | cam0、cam1 分别约 30.02 fps，均可保存 18,000,000 字节的 NV12 帧 |
| 双路 RAW 4000×3000 RG10 | 通过（短时） | 两路并发各采 300 个有效帧，约 30.02 fps，无丢帧、无超时 |
| 双路 ISP/NV12 4000×3000 | 失败，根因已定位 | 干净复现得到 cam0 15 帧、cam1 14 帧；两路在 frame-id 13 曝光查找失败，AIQ 以 SIGSEGV 退出 |
| 双路 2～4 Hz | 未通过 | 当前尝试写 VBLANK 后仍被 RKAIQ/流启动过程改变，不能证明目标帧率已生效 |
| 两路独立 ISP 上下文 | 基础存在 | media2/media3 是独立 ISP；当前一个 3A 服务创建两个相机上下文 |
| 两路独立 IQ 文件 | 未完成 | 两路当前实际都加载 `/etc/iqfiles/imx586_default_default.json` |
| UART 相机控制协议 | 未实现/未测试 | 当前只验证了串口设备和串口参数，尚无阶段 4 控制程序可做端到端测试 |
| 30～60 分钟稳定性 | 暂不执行 | 双路 ISP 会在几秒内失败，应先修复后再做长稳测试 |

## 3. 先理解当前软件结构

当前链路如下：

```text
IMX586 cam0 -> MIPI/CIF0 -> rkisp-vir0(/dev/media2) -> /dev/video22 -> 应用
                            ^
                            | RKAIQ context 0

IMX586 cam1 -> MIPI/CIF1 -> rkisp-vir1(/dev/media3) -> /dev/video31 -> 应用
                            ^
                            | RKAIQ context 1

一份 rkaiq_3A_server 进程负责发现两个 ISP，并为它们分别创建上下文。
当前两个上下文都读取同一份 imx586_default_default.json。
```

这里必须区分两个概念：

1. **AIQ 服务程序/进程**：`/usr/bin/rkaiq_3A_server`。一个进程可以管理两个独立 ISP 上下文，因此“一台设备只有一个 AIQ 服务进程”本身不是错误。
2. **IQ 数据文件**：包含 AE、AWB、BLC、LSC、降噪等标定参数。当前两路使用同一个文件，只能作为初期跑通基线，不能满足文档中“广角、鱼眼分别保留 IQ 配置”的最终验收条件。

不要直接启动两个完全相同的 `rkaiq_3A_server` 实例。当前程序会扫描多个 media 设备，没有按相机限定资源的命令行参数；两个实例可能同时抢占两路 ISP。若后期供应商给的是两个独立 AIQ 可执行程序，它们必须能够分别固定绑定 media2/media3、Sensor 实体和各自 IQ 文件，才能采用双进程方式。

## 4. 串口验证结果

### 4.1 `/dev/ttyUSB0` 到底是什么

`/dev/ttyUSB0` 是主机侧设备名，USB 芯片识别为 CH340（USB ID `1a86:7523`）。它连接到 RK3576 的调试控制台：

```text
主机 /dev/ttyUSB0  <--USB/TTL-->  RK3576 /dev/ttyFIQ0
```

本次已使用它登录板端并完成相机命令测试。RK3576 启动参数也包含 `console=ttyFIQ0`。实测连接参数为：

```text
1500000 baud, 8 data bits, no parity, 1 stop bit
```

因此不能把主机 `/dev/ttyUSB0` 直接当作需求中“115200 8N1 的 UART 控制口”。如果把当前调试口改成 115200，会失去或打乱现有控制台通信。

### 4.2 阶段 4 应使用的业务 UART

板端当前可见且未被 getty 占用的普通串口是 `/dev/ttyS6`，对应 `2ad90000.serial`。本次已临时执行：

```bash
stty -F /dev/ttyS6 115200 cs8 -cstopb -parenb \
  -ixon -ixoff -crtscts raw -echo
stty -F /dev/ttyS6 -a
```

配置成功，确认可以进入 115200、8N1、无硬件流控、RAW 模式。测试后已用原始 `stty -g` 值恢复，未永久修改板端配置。

本次没有做 `/dev/ttyS6` 的物理 TX/RX 测试，原因是当前唯一的 USB 转串口线接在调试口，而不是 ttyS6 引脚。要完成物理验证，需要第二个 USB-UART：

```text
USB-UART TX  -> RK3576 ttyS6 RX
USB-UART RX  <- RK3576 ttyS6 TX
USB-UART GND -- RK3576 GND
```

注意 TX/RX 交叉、GND 共地，电平必须与板卡接口一致，不能把 RS-232 电平直接接到 3.3 V TTL 引脚。

### 4.3 UART 程序位于哪一层

阶段 4 的 UART 控制程序属于 **Linux 应用层常驻服务**，不是 IMX586 内核驱动的一部分。职责边界应该是：

```text
UART 收包
  -> 帧头/长度/CRC/命令/camera_id 校验
  -> 相机控制服务分发
      -> 曝光、增益、白平衡：RKAIQ API
      -> 帧率、标准 V4L2 control：V4L2 ioctl
      -> 开始/停止保存：采集与存储模块
      -> UVC/网络/eMMC：各输出后端
  -> 返回 ACK、错误码和状态
```

UART 应用不能直接通过 I2C 裸写 IMX586 寄存器。寄存器时序、stream 状态和 3A 状态由 Sensor 驱动、V4L2 和 RKAIQ 协调；应用绕过它们裸写，容易造成 AIQ 缓存参数与硬件实际状态不一致。

## 5. 相机硬件与 media graph 验证

### 5.1 Sensor 和 CIF

检测到两颗 IMX586：

| camera_id | Sensor 实体 | Sensor subdev | CIF media | RAW 节点 |
| --- | --- | --- | --- | --- |
| 0 | `m00_b_imx586 4-001a` | `/dev/v4l-subdev4` | `/dev/media0` | `/dev/video0` |
| 1 | `m01_b_imx586 5-001a` | `/dev/v4l-subdev9` | `/dev/media1` | `/dev/video11` |

两路 Sensor 当前均报告：

- 分辨率：4000×3000
- RAW 格式：`MEDIA_BUS_FMT_SRGGB10_1X10`
- 曝光 control：2～3060
- 模拟增益 control：16～1024
- VBLANK：64～29767，当前和默认值均为 64
- HBLANK：4976，只读
- pixel rate：320 MHz，只读
- link frequency：400 MHz/625 MHz

### 5.2 ISP 输出节点

| camera_id | ISP media | mainpath | 稳定别名 | 当前格式 |
| --- | --- | --- | --- | --- |
| 0 | `/dev/media2` | `/dev/video22` | `/dev/video-camera0` | 4000×3000 NV12 |
| 1 | `/dev/media3` | `/dev/video31` | `/dev/video-camera1` | 4000×3000 NV12 |

当前别名有利于测试，但量产应用不要把 `/dev/video22` 和 `/dev/video31` 写死。应根据 media graph 中的 Sensor 实体、`module-index` 或 udev 规则解析 camera_id，避免节点编号在驱动或启动顺序变化后改变。

## 6. 图像采集测试

### 6.1 单路 ISP/NV12 测试：通过

cam0 和 cam1 分别以 4000×3000 NV12 采集，先跳过 10 帧，再统计 60 帧：

```bash
v4l2-ctl -d /dev/video-camera0 \
  --set-fmt-video=width=4000,height=3000,pixelformat=NV12 \
  --stream-mmap=4 --stream-skip=10 --stream-count=60 --verbose

v4l2-ctl -d /dev/video-camera1 \
  --set-fmt-video=width=4000,height=3000,pixelformat=NV12 \
  --stream-mmap=4 --stream-skip=10 --stream-count=60 --verbose
```

实测结果：

| 项目 | cam0 | cam1 |
| --- | --- | --- |
| 测试耗时 | 约 2499 ms | 约 2507 ms |
| 相邻帧时间 | 约 33.31 ms | 约 33.31 ms |
| 估算帧率 | 约 30.02 fps | 约 30.02 fps |
| 结果 | 通过 | 通过 |

另外分别保存了一帧预热后的图像，两帧均为 18,000,000 字节，符合 4000×3000 NV12 的大小：

```text
cam0 SHA-256: c926ad3109d18942621be839c1aca62222d62351aa0bc7ef7ccfd2102884afa2
cam1 SHA-256: e9c326e54b6178ed39eecb53689b243960fc3b885e680dca63ee9b3f41bd7d40
```

两帧和多个抽样块的哈希均不同，说明两个节点不是重复指向同一块静态缓冲区。

### 6.2 双路 ISP/NV12 并发测试：失败，崩溃点已确认

为排除 systemd、旧进程和启动顺序干扰，停止服务并清理所有 AIQ/采集进程后，直接启动一个 `/usr/bin/rkaiq_3A_server`，再同时请求两路 4000×3000 NV12、各 120 帧。结果为：

- cam0：实际得到 15 帧后超时；
- cam1：实际得到 14 帧后超时；
- 两路都在 frame-id 13 报曝光参数查找失败；
- `rkaiq_3A_server` 返回 139，即收到 SIGSEGV；
- 两个采集进程因 AIQ/ISP 参数链路停止而超时，不是 `v4l2-ctl` 主动完成。

两路最后的关键错误一致：

```text
can't find the latest effecting exposure for id 13, impossible case
XCORE:E:fid:13 fail to get expParams
frame_id(13), get exposure failed!!!
```

板端 core dump 已保存在 `/tmp/codex_aiq_core2/core`。GDB 回溯为：

```text
_setIspConfig()
  -> AiqCamHw_handleIspRstList()
  -> AiqCamHw_applyAnalyzerResultList()
  -> AiqManager_applyAnalyzerResult()
  -> AiqCore_groupAnalyze()
  -> AiqAnalyzerGroup_msgHandle()
```

崩溃指令位于 `_setIspConfig+380`，当时 `x0=0`，下一条指令为 `ldr s0, [x0,#40]`。这证明 AIQ 正在通过空曝光指针读取成员，并非推测性的“服务主动退出”。

启动期间内核还出现过：

```text
csi size err
MIPI_CSI2 ERR2 0xf0000
online mode vblank need >= 1000us, current 696us
```

注意：此次 `v4l2-ctl` 最终返回值是 0，但不能据此判定通过。该工具发生 `select timeout` 时仍可能返回 0。验收脚本必须同时检查：

1. 实际 DQBUF 数是否达到请求数量；
2. 是否出现 `select timeout`；
3. sequence 是否连续；
4. `rkaiq_3A_server` 是否仍存活；
5. `dmesg` 是否新增 CIF/MIPI/ISP 错误。

### 6.3 双路 RAW/RG10 并发测试：通过（短时）

为了区分“物理采集链路问题”和“ISP/AIQ 问题”，停止 AIQ 后绕过 ISP mainpath，直接从两个 CIF RAW 节点并发取流：

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=4000,height=3000,pixelformat=RG10 \
  --stream-mmap=4 --stream-skip=10 --stream-count=300 --verbose

v4l2-ctl -d /dev/video11 \
  --set-fmt-video=width=4000,height=3000,pixelformat=RG10 \
  --stream-mmap=4 --stream-skip=10 --stream-count=300 --verbose
```

两条命令并发执行。结果如下：

| 项目 | cam0 RAW | cam1 RAW |
| --- | --- | --- |
| 有效帧 | 300 | 300 |
| 跳过帧 | 10 | 10 |
| 最后 sequence | 309 | 309 |
| 每帧 bytesused | 15,360,000 | 15,360,000 |
| 测试耗时 | 约 10.465 s | 约 10.476 s |
| 估算帧率 | 约 30.02 fps | 约 30.02 fps |
| 丢帧/超时 | 0/0 | 0/0 |

该结果说明两颗 IMX586、两路 MIPI、CIF 和短时内存写入链路具备并发工作的基础。它不能替代 ISP/NV12 验收，也不能替代 30～60 分钟稳定性测试，但能把当前故障重点缩小到双 ISP 参数队列、RKAIQ 曝光时序或相关启动流程。

## 7. IQ 与 RKAIQ 验证

板端现有两份名字不同的 IMX586 文件：

| 路径 | 大小 | SHA-256 |
| --- | ---: | --- |
| `/etc/iqfiles/imx586_default_default.json` | 约 2.3 MB | `09e959d3a9e978da814d7924f8dcd64ae3e1053d1f91be8a529ce79ff95006c8` |
| `/etc/iqfiles/imx586.json` | 约 658 KB | `b1a02c7cf57eef51e57ab1d6e5a5fbc1b0bbad77f3f72877b9335e338861e323` |

实际启动日志表明，cam0 和 cam1 当前都加载第一份 `imx586_default_default.json`。`imx586.json` 存在不代表它被使用。

还要注意：这两份文件目前存在于板端，但在本次检查的 SDK IQ 目录和 rootfs overlay 中没有找到相同的 IMX586 IQ 文件。也就是说，当前运行板和 SDK 构建输入并不完全一致；重新制作镜像时可能丢失现有 IQ。下一次构建前应把经确认的 IQ 文件加入受版本管理的 rootfs overlay，并在镜像验收时再次核对 SHA-256。

SDK 中对应逻辑会根据 Sensor、module、lens 名拼接 IQ 文件名。当前两路 DTS 的 module/lens 都是 `default`，所以自然选择同一个文件。相关位置：

- `external/camera_engine_rkaiq/rkaiq/hwi/isp20/CamHwIsp20.cpp`
- `external/camera_engine_rkaiq/rkaiq_3A_server/rkaiq_3A_server.cpp`
- `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi0.dtsi`
- `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi1.dtsi`

SDK 对应的 `rkaiq_3A_server.cpp` 原始默认 prepare 尺寸为 2688×1520。当前工作区已被手工改为 `width=3000`、`height=4000`，但宽高仍然写反；正确固定值应为 `width=4000`、`height=3000`。更稳妥的量产做法是从每路 media/Sensor 当前格式读取真实宽高，再分别 prepare。该程序的 `getopt_long()` 也没有正确声明宽高参数，初始化和 start 返回值检查不完整，需要一并修正，但这些不是本次空指针崩溃的直接原因。

当前一份 IQ 可以继续作为 bring-up 基线，但必须满足两个限制：

- 只能用来证明双路 pipeline 能运行，不能验收左右独立画质和参数互不串扰；
- 不能因为“使用同一份 IQ”就运行两个相同 AIQ server，仍应由一个服务管理两个上下文。

## 8. 为什么写 VBLANK 仍没有得到 2～4 Hz

本次曾把两个 Sensor 的 `vertical_blanking` 临时设为 19980，control 写入本身返回成功。随后启动 AIQ 和双路采集，实测帧间隔仍约 33.31 ms，约 13～14 帧后再次超时，AIQ 仍因曝光参数查询失败退出。测试结束后两个 VBLANK 已恢复为 64。

这说明当前不能在 AIQ 启动前简单写一次 VBLANK 就认为帧率已设置完成。流启动和 AIQ prepare 过程可能重新配置 Sensor 时序。正确实现应满足：

1. 驱动明确提供 4000×3000 目标低帧率模式，或正确实现 `V4L2_CID_VBLANK`/frame interval；
2. 相机控制服务在正确的 stream/AIQ 生命周期阶段设置帧率；
3. 启动后再次读取 control 和实际帧时间戳，不能只检查 ioctl 返回值；
4. AE 的最大曝光时间必须受新的帧周期约束；
5. 两路同时测量至少 1000 帧，确认平均频率在 2～4 Hz 且没有丢帧。

不建议仅根据 pixel_rate、HBLANK、VBLANK 的表面值自行计算并直接写一个“大概的 VBLANK”。应先核对 IMX586 当前 mode 的 HTS/VTS、驱动单位、lane 配置和寄存器实现，再以真实帧时间戳闭环验证。

## 9. 已确认的根因与修复方案

### 9.1 为什么单路成功、双路失败

单路时，AIQ 分析结果通常能在曝光历史被清理前完成；双路 12 MP、30 fps 时，一个 AIQ 进程同时处理两个 ISP 上下文，分析/参数应用相对 SOF 的滞后变大。当前 C 实现只保留约 5 帧 `_effecting_exp_map` 历史，而旧 C++ 实现保留约 10 帧。到 frame-id 13 时，需要的旧曝光项已经被清理，曝光查找失败。

当前 C 查找实现还有第二个退化：精确 frame-id 不存在且找不到更旧项时，它直接返回 `NULL`；旧 C++ 实现只要 map 非空，就会选择一个可用项并打印 warning。最后，ISP39 的 `_setIspConfig()` 在收到 `NULL` 后只打印错误，仍无条件调用 `_setIspExp(..., expParam)`，最终空指针崩溃。

因此完整因果链是：

```text
双路处理滞后增加
  -> 5 帧曝光历史不足
  -> frame-id 13 查找返回 NULL
  -> ISP39 无条件调用 _setIspExp(NULL)
  -> rkaiq_3A_server SIGSEGV
  -> ISP 参数停止下发
  -> 两路 NV12 select timeout
```

严格来说，core 已经完全确认“返回 NULL 后空指针崩溃”；“5 帧历史不足且缺少旧版回退”是结合新旧源码差异得到的高可信根因。修改后应临时打印 camera-id、请求 frame-id、map 大小及最小/最大 key，完成最终运行时闭环确认。

### 9.2 必须修改的三组代码

第一组是防崩溃保护，位置在 `rkaiq/hwi_c/aiq_CamHwBase.c` 的 ISP39 分支。只有 `expParam != NULL` 时才能调用 `_setIspExp()`：

```c
#if defined(ISP_HW_V39)
if (expParam) {
    _setIspExp(pCamHw, &isp_params->exposure, expParam);
} else {
    LOGW_CAMHW_SUBM(ISP20HW_SUBM,
                    "cam%d fid:%u skip missing exposure metadata",
                    pCamHw->mCamPhyId, frameId);
}
#endif
```

这只是必要的安全保护，不能单独作为最终修复，因为它会跳过该帧的曝光 metadata。

第二组是恢复曝光队列的容错能力，位置在 `rkaiq/hwi_c/aiq_sensorHw.c`：

1. `_SensorHw_handleSofInternal()` 和 `SensorHw_handle_sof()` 中的 map 清理阈值都从 `> 5` 恢复为旧实现的 `> 10`；
2. `SensorHw_getEffectiveExpParams()` 在精确项和不晚于请求帧的项都不存在时，如果 map 非空，恢复旧实现的 fallback 行为，而不是返回 `NULL`；
3. 临时日志加入 `cam-id/search-id/map-size/min-key/max-key`，验证双路时队列到底滞后多少帧；
4. 只有 map 真为空时才返回 `NULL`，并保留第一组判空作为最后防线。

map 默认池容量为 20，旧实现已使用 10 帧阈值，因此先恢复到 10 是比任意扩大队列更有依据的选择。fallback 的具体 key 选择应与本 SDK 旧 C++ 版本保持一致，不要自行用固定曝光值替代。

第三组是修复 server 生命周期和模式参数，位置在 `rkaiq_3A_server/rkaiq_3A_server.cpp`：

1. 当前固定宽高改正为 4000×3000，随后改为按每路实际格式获取；
2. 修复宽高命令行参数声明，避免 `-h` 与 help 冲突；
3. `rk_aiq_uapi2_sysctl_init()` 后先判空，再调用后续 API；
4. 检查 prepare/start 返回值，任一路失败时停止已启动 context 并明确退出；
5. 两个 context 都 init/prepare/start 完成后，再允许采集应用同时 STREAMON。

### 9.3 修复后的构建与替换原则

在当前 SDK 使用已有 RK3576/ISP39 Buildroot 配置重编 `camera-engine-rkaiq`，再更新板端 `/lib/librkaiq.so` 和 `/usr/bin/rkaiq_3A_server`。库和可执行程序必须来自同一次构建，替换前记录原文件 SHA-256 并保留可恢复副本。调试时开启 core dump 和完整 AIQ 日志，不要先用 systemd 无限重启掩盖崩溃。

当前 systemd unit 是 `Type=forking`，启动脚本通过管道把日志送到 logger，且没有 `Restart=` 策略。根因修复后再改为可准确跟踪主进程的服务形式，并配置有限的 `Restart=on-failure` 和失败告警。

### 9.4 修复后的测试顺序

1. cam0 单路 100 帧、cam1 单路 100 帧，确认无回归；
2. 双路同时各 1000 帧，必须按实际 DQBUF 计数，不以进程返回 0 代替；
3. 检查两个 sequence、timeout、AIQ 进程、AIQ 错误日志和 dmesg 增量；
4. 日志确认两路所有请求 frame-id 都能得到曝光项，fallback 不应持续高频出现；
5. 再跑双路各 10,000 帧；
6. 最后执行 30～60 分钟长稳，并统计队列最大滞后、丢帧和温度。

### 优先级 2：处理在线模式 VBLANK 警告

内核提示当前 696 us 小于建议的 1000 us。应检查 ISP online 模式、Sensor 时序和低帧率工作方式。虽然 RAW 双路通过表明它不是当前唯一根因，但该警告可能缩小双 ISP 的调度余量，不能在最终版本中忽略。

### 优先级 3：复查启动瞬间 MIPI/CIF 错误

双 ISP 失败测试启动时出现过 `csi size err` 和 `MIPI_CSI2 ERR2 0xf0000`。后续双 RAW 300 帧期间未观察到新的相机链路错误，因此暂不把它列为主因。修复 AIQ 后仍要逐次清空/记录 dmesg 增量，确认它不会重复发生。

## 10. 从现在开始的实施顺序

### 第一步：保留当前可恢复基线

记录当前 DTS、内核、rootfs、AIQ 二进制和 IQ 文件哈希。保留以下已知状态：

- cam0 单路 ISP 可用；
- cam1 单路 ISP 可用；
- 双 RAW 短时可用；
- 双 ISP 会快速失败；
- 两路当前共享 `imx586_default_default.json`。

### 第二步：修复双路 ISP/RKAIQ

这是当前首要开发项。先不写 UART 相机业务命令。按第 9 节定位并修复服务退出，验收门槛为双路 NV12 4000×3000 同时连续取得 1000 帧，两个 sequence 无跳变、无 timeout、AIQ 服务存活、dmesg 无新增错误。

### 第三步：实现并验证 2～4 Hz

在双 ISP 稳定后，再把两路都设置到目标低帧率。依次验证单路 cam0、单路 cam1、双路，记录真实时间戳而不是只看 `--get-parm`。同步核对曝光上限和 AE 是否收敛。

### 第四步：做左右参数独立性测试

即使暂时共享 IQ，也要证明两个 AIQ context 的运行时控制互不串扰：固定场景下只改变 cam0 曝光/增益，cam1 的 control、统计值和画面亮度不应随之改变；然后交换测试。此测试通过后，UART 的 `camera_id` 才有可靠基础。

### 第五步：接入阶段 4 UART 控制应用

UART 应用放在独立的产品应用/服务目录，由它统一持有两路相机资源。不要因为使用 RKAIQ 就把程序放进 `external/camera_engine_rkaiq`；只有修改/扩展 RKAIQ 库或官方 AIQ server 本身时才应该改那个目录。

应用至少分为：

```text
uart_transport     串口打开、收发、缓存、超时
protocol_parser    帧格式、CRC、camera_id、命令和错误码
camera_manager     cam0/cam1 状态机与资源所有权
aiq_control        曝光、增益、AWB 等 RKAIQ 调用
v4l2_capture       格式、帧率、取帧、时间戳
storage/output     eMMC、UVC、网络输出
status             帧率、丢帧、温度、错误和服务状态
```

### 第六步：长稳和故障恢复

双 ISP 1000 帧、低帧率和独立控制通过后，再执行 30～60 分钟测试。至少记录：

- 每路总帧数、实际 FPS、最小/最大/平均帧间隔；
- sequence 丢帧数和重复帧数；
- MIPI/CIF/ISP/AIQ 错误增量；
- CPU、DDR、ISP 占用和温度；
- AIQ、采集应用是否重启；
- 拔掉一只相机、错误命令、存储写满后的恢复行为。

## 11. 后期切换到两路独立 IQ 的方案

推荐仍使用一个相机服务进程、两个 AIQ context、两份 IQ 文件：

```text
cam0 entity -> AIQ context 0 -> imx586_left_wide.json
cam1 entity -> AIQ context 1 -> imx586_right_fisheye.json
```

可选实现有两种。

### 方案 A：通过 DTS 身份自动选文件

为两路设置稳定且不同的 `rockchip,camera-module-name` 和 `rockchip,camera-module-lens-name`，并按 RKAIQ 的命名规则放置匹配文件。优点是沿用现有自动发现逻辑，改动小；缺点是文件名和 DTS 强耦合，必须把映射写入版本记录。

### 方案 B：应用显式指定 IQ 文件

自定义相机服务为每个 Sensor entity 调用 RKAIQ preInit/初始化接口，明确传入各自 IQ 路径，再创建两个 context。优点是映射清晰、便于升级和回滚；缺点是需要替换或扩展当前简化的 `rkaiq_3A_server` 启动方式。

如果后期坚持运行两个独立 AIQ 可执行程序，则每个程序必须新增类似以下启动参数，并从代码上保证只扫描/打开指定设备：

```text
aiq_cam0 --media /dev/media2 --sensor 'm00_b_imx586 4-001a' --iq left.json
aiq_cam1 --media /dev/media3 --sensor 'm01_b_imx586 5-001a' --iq right.json
```

当前 `rkaiq_3A_server` 没有这种完整的设备/IQ 隔离参数，因此现在不能用复制进程的办法代替双 IQ 设计。

## 12. UART 程序完成后的测试方法

### 12.1 最小协议建议

无论使用文本还是二进制协议，至少要包含：

```text
magic | version | command | camera_id | sequence | payload_len | payload | CRC16
```

所有响应应回显 `command`、`camera_id` 和 `sequence`，并返回明确错误码。禁止只回 `OK`，否则双相机并发时无法判断响应属于哪条命令。

### 12.2 测试顺序

1. 第二个 USB-UART 接到 ttyS6，先做 TX/RX 回环和双向固定字符串测试；
2. 验证粘包、半包、错误帧头、错误长度和错误 CRC 不会使进程崩溃；
3. 测 `camera_id=0`、`camera_id=1` 和非法 id；
4. 分别读写曝光、增益、帧率，写后通过 RKAIQ/V4L2 回读；
5. 改 cam0 时确认 cam1 不变，改 cam1 时确认 cam0 不变；
6. 测开始/停止保存、重复开始、未开始就停止、存储写满；
7. 测状态查询中的帧率、丢帧、最近错误、AIQ 状态；
8. 最后才接 UVC、网络和 eMMC 多后端并发。

### 12.3 每条控制命令的通过条件

以“设置曝光”为例，不能只以 UART 收到 ACK 为通过。完整闭环是：

```text
主机发送 camera_id=0, exposure=目标值
  -> UART CRC/范围校验通过
  -> AIQ/V4L2 设置成功
  -> 回读值处于允许误差内
  -> cam0 后续帧 metadata/亮度发生预期变化
  -> cam1 参数与画面没有非预期变化
  -> 返回带 sequence 的成功响应
```

## 13. 本次测试后的板端状态

- 两路 Sensor 的 VBLANK 均已恢复为 64；
- 临时采集进程已结束；
- `rkaiq_3A.service` 已恢复为 active；
- 崩溃 core 保留在板端 `/tmp/codex_aiq_core2/core`，重启前应复制归档；
- 服务恢复后，cam0 和 cam1 再次分别完成 30 帧顺序采集，无 timeout；
- 板端 `/tmp` 中保留的测试文件重启后会丢失，不应当作长期测试档案；
- 板端系统时间显示为 2026-06-06，与主机测试日期 2026-07-16 不一致；正式日志/时间戳测试前必须校时；
- 板端当前无 IPv4，内核有 GMAC 初始化失败日志。该问题不是本次相机失败的直接证据，但会阻塞后续网络输出和 NTP 校时阶段。

## 14. 下一轮验收清单

| 序号 | 验收项 | 通过条件 |
| --- | --- | --- |
| 1 | 双路 NV12 1000 帧 | 两路均达到请求帧数，0 timeout，0 sequence 跳变，AIQ 存活 |
| 2 | 内核错误 | 测试窗口内无新增 MIPI/CIF/ISP error |
| 3 | 2～4 Hz | 两路实测帧时间戳均在目标范围，曝光上限正确 |
| 4 | 控制隔离 | 只改一侧曝光/增益/帧率，另一侧 control 和图像不受影响 |
| 5 | 独立 IQ | 两路日志明确显示加载不同 IQ，AE/AWB 独立收敛 |
| 6 | UART 物理链路 | ttyS6 115200 8N1 双向收发、CRC 和异常帧测试通过 |
| 7 | UART 命令闭环 | camera_id=0/1 全部命令可设置、可回读、响应可关联 |
| 8 | 长稳 | 30～60 分钟无崩溃，丢帧和温度满足产品指标 |

本轮结论是：**双相机硬件采集基础成立，双路失败不是 RK3576 或两颗 IMX586 不能并发，而是 RKAIQ C 实现的曝光历史/回退退化触发 ISP39 空指针崩溃。应先完成判空、恢复 10 帧历史和 fallback、修正 4000×3000 prepare，再按单路 100 帧、双路 1000 帧、10,000 帧和长稳逐级验收；通过后再做低帧率、控制隔离和 UART 应用。**
