# RK3576 双 IMX586 v0.2 逐项实机复测报告

测试日期：2026-08-10  
需求依据：`RK3576 MIPI 相机模组任务计划 v0.2`  
被测硬件：RK3576、两颗 IMX586、STM32F103 XVS 发生器  
被测程序：`/root/camera_uart/camera_aiq_test`  
程序 SHA256：`c6cdd3064a1509ad2175590d2e2e68fe3016758fac0d656f296d0bb26c282a58`  
本轮证据目录：`test_results/20260810_v02_requirement_retest`  
板端证据目录：`/root/camera_uart/test_results/20260810_v02_requirement_retest`

## 1. 先看最终结论

| 阶段 | 本轮判定 | 最关键的实测结果 |
| --- | --- | --- |
| 1. 双路出图和映射 | 通过 | 双路 4000x3000 约 2 Hz，短时连续采集无 sequence 丢帧，单停 cam1 不影响 cam0 |
| 2. 独立 ISP/IQ 和参数 | 部分通过 | 曝光、增益、ISO 的独立设置和最终回读通过；低帧率时命令会先误报校验超时；两份 IQ 内容相同，AWB/BLC/LSC/LDC 独立调校未验收 |
| 3. 2-4 Hz、稳定性、资源和功耗 | 部分通过，目标未全部达到 | STM32 物理输入约 2 Hz；`1/1` 时两路真实约 2 Hz，`1/2` 时约 1 Hz；真实 4 Hz和四组合不通过；功耗和长稳未测 |
| 4. 驱动和 UART 控制 | 部分通过 | 控制程序和协议自测通过，RK3576 发出 38 字节；STM32 回传为 0 字节，真实双向 UART 不通过 |
| 5. PPS/NMEA/Trigger 硬件同步 | 部分通过 | 两路 XVS 从模式约 2 Hz 通过，软件模拟绑定通过；真实 PPS、UTC、Trigger 事件和曝光起点精度未验收 |
| 6. EXIF 和照片元数据 | 部分通过 | 双路实际 JPEG 的 EXIF、UserComment、frame_id、trigger_id 均存在；当前是模拟触发且 UTC 无效 |
| 7. UVC/网口/eMMC | 部分通过 | 双 UVC、双 HTTP、RNDIS ping/SSH和eMMC均实测通过且可共存；实际链路为 USB2.0 480M，不是 USB3.0；UVC启动瞬态有跳号 |

因此不能写成“只有 MCU 没测，其余全部通过”。目前还剩五类正式验收缺口：

1. STM32 UART 双向通信以及 MCU 切换到共享 4 Hz。
2. 四种帧率组合 `4/4`、`4/2`、`2/4`、`2/2` 的真实物理帧率。
3. 真实 PPS、GPRMC/GNRMC、XVS trigger_id 和 UTC 锁定。
4. 两颗传感器曝光起点同步精度的外部测量。
5. USB3.0物理链路、UVC启动瞬态，以及长稳和功耗测试。

## 2. 本轮测试边界

当前 STM32 的 XVS 线已经接入，两颗 IMX586 都运行在 XVS Slave 模式。实测 XVS 是固定约 2 Hz。STM32 UART 没有任何返回，所以无法查询其固件版本、当前频率，也无法命令它切换到 4 Hz。

本轮没有接入或没有得到有效数据的部分：

- 没有真实 PPS 和 GNSS GPRMC/GNRMC 数据；
- 没有 MCU 上报的真实 `EVT,XVS` 和 `timer_tick`；
- 没有功耗仪；
- 没有执行 30-60 分钟长稳；
- 板卡系统时间错误，板端显示 2026-06-05，而实际测试日期为 2026-08-10。

USB补测时电脑已接入，复合设备成功枚举为两组UVC加RNDIS。板端UDC为`configured`，但`current_speed=high-speed`，电脑拓扑也显示480M，因此只能判定USB2.0已连通，不能判定USB3.0通过。

根分区约 96% 已使用，仅剩约 1.2 GiB。本轮只短时保存一帧 18 MB NV12，避免写满 eMMC。

## 3. 固定设备关系

| camera_id | Sensor | ISP 采集节点 | AIQ params | IQ 目录 |
| ---: | --- | --- | --- | --- |
| 0 | `m00_b_imx586 4-001a` | `/dev/video22` | `/dev/video29` | `/etc/iqfiles/cam0` |
| 1 | `m01_b_imx586 5-001a` | `/dev/video31` | `/dev/video38` | `/etc/iqfiles/cam1` |

两份 IQ 文件当前 SHA256 都是：

```text
d0f4a039190140e17995441f72dd162c0157e85ddea808281d3010cb508bb027
```

这说明程序确实创建了两个独立 AIQ context，并从两个路径加载文件，但两份 JSON 内容相同。它不等价于“广角和鱼眼已分别完成 IQ 标定”。

## 4. 阶段 1：双路出图和固定映射

### 4.1 做了什么

启动两路采集，等待 6 秒读取帧计数；然后只停止 cam1，确认 cam0 继续增长；最后恢复 cam1，再次读取两路计数。

程序控制台命令：

```text
stream-start all
wait 6000
capture-status all
status all
sync-status
stream-stop 1
wait 6000
capture-status 0
stream-start 1
wait 6000
capture-status all
```

### 4.2 实测结果

| 工况 | cam0 | cam1 |
| --- | --- | --- |
| 首次双路 6 秒 | 13 帧，2.093 Hz，sequence 0-12，drop=0 | 13 帧，2.089 Hz，sequence 0-12，drop=0 |
| 停止 cam1 后 | cam0 累计 26 帧，2.043 Hz，drop=0 | 已停止 |
| 恢复双路后 | 约 2.000 Hz，drop=0 | 约 2.070 Hz，drop=0 |

### 4.3 判定

阶段 1 通过。映射固定、两路能同时出图、单路启停不串扰、短时无 sequence 跳号。

`SYNC_STATUS delta_ns=13000-17000` 只表示两个 V4L2 完成时间接近，不能证明两个传感器的曝光起点只差 13-17 us。

## 5. 阶段 2：独立参数和独立 IQ

### 5.1 做了什么

先给两路设置明显不同的曝光和增益，再只修改其中一路，检查另一条是否保持不变，最后测试 ISO 接口。

```text
auto 0
auto 1
wait 3000
exposure 0 5000
gain 0 2000
exposure 1 20000
gain 1 8000
wait 3000
status all
exposure 0 10000
wait 3000
status all
gain 1 4000
wait 3000
status all
iso 0 150
iso 1 500
wait 3000
status all
```

### 5.2 实测结果

| 操作 | cam0 回读 | cam1 回读 | 结论 |
| --- | --- | --- | --- |
| 初始独立设置 | 5004 us、2x、ISO 100 | 19996 us、8x、ISO 400 | 两路均生效 |
| 只改 cam0 曝光 | 9998 us、2x | 保持 19996 us、8x | 曝光不串扰 |
| 只改 cam1 增益 | 保持 9998 us、2x | 19996 us、4x | 增益不串扰 |
| 设置 ISO | ISO 150，对应 3x | ISO 500，对应 10x | 两路均独立生效 |

所有上述状态均为 `manual_settings_verified=1`。

本轮USB补测期间，传感器一度处于约1 fps。此时`exposure`、`gain`和`iso`命令会先返回`code=-7 manual setting did not reach requested value`，但等待约3秒后实际回读值全部正确并显示`manual_settings_verified=1`。这说明硬件设置最终生效，但程序的即时校验窗口短于低帧率下的新帧到达时间。产品程序应按当前实测帧周期动态延长校验超时，或将命令返回改成“已提交”，再异步报告最终状态。

### 5.3 `actual_iso` 和 `estimated_iso` 的解释

本平台 RKAIQ 查询结果中 `aiq_iso=0`，所以程序用基础 ISO 50 乘以实际总增益计算：3x 得到 ISO 150，10x 得到 ISO 500。状态中的 `iso_estimated=1` 是在诚实说明这个来源。

需求中的“设置 ISO/增益”已经能完成，因为 ISO 命令最终转换为实际增益并回读验证；但不能把这个值称为 RKAIQ 原生 `actual_iso`。如果产品定义 ISO 就是标定后的增益换算值，需要把基础 ISO 和换算曲线写入每颗相机的标定文件并固定算法。

### 5.4 判定

曝光、增益和 ISO 的独立控制通过。阶段 2 整体只能判部分通过，因为：

- 两份 IQ JSON 内容完全相同；
- 本轮没有分别改变并检查 AWB、黑电平、LSC、LDC；
- 没有广角/鱼眼标定图和 IQ 工程师验收数据。

## 6. 阶段 3：2-4 Hz、稳定性、资源和功耗

### 6.1 做了什么

依次请求四种组合：

```text
fps 0 4
fps 1 4
wait 6000
capture-status all
fps 0 4
fps 1 2
wait 6000
capture-status all
fps 0 2
fps 1 4
wait 6000
capture-status all
fps 0 2
fps 1 2
wait 6000
capture-status all
```

### 6.2 实测结果和失败原因

内核明确打印两颗传感器都进入：

```text
XVS slave mode enabled
```

STM32 当前送来的共享 XVS 约为 2 Hz。因此：

- 请求 2 Hz 时，传感器最多按 2 Hz 输出；
- 请求 4 Hz 时，上游一秒只有两个 XVS 边沿，软件不可能凭空得到四帧；
- 程序正确返回 `target fps did not stabilize`，没有把请求值伪装成实测值。

USB补测进一步确认了input-thin关系：

| STM32实际XVS | 状态中的 `xvs_input_thin` | 5秒帧计数变化 | 实际输出 |
| --- | ---: | ---: | ---: |
| 约2 Hz | 1，即1/2输入抽帧 | 稳态约5帧 | 约1 Hz |
| 约2 Hz | 0，即1/1接收 | cam0 `246 -> 256`，cam1 `214 -> 224` | 两路均约2 Hz |

所以USB最初约1 fps不是USB带宽造成，而是传感器输入侧对2 Hz XVS又做了一次1/2抽帧。执行`fps 0 4`和`fps 1 4`后，驱动切到`xvs_input_thin=0`，虽然因为没有4 Hz物理输入而正确报告“4 Hz未稳定”，但两路真实输出恢复到上游能够提供的约2 Hz。常驻服务恢复后，电脑端两路UVC稳态帧间隔也都约500 ms。

`fps_x1000=0` 出现在流重启后的单帧统计窗口中，表示只有一个时间点、暂时算不出间隔，不表示相机没有帧。持续观察帧计数仍会增长。

### 6.3 正确实现方式

STM32 应固定输出共享 4 Hz XVS。每颗 IMX586 再独立选择：

| 目标帧率 | IMX586 XVS input-thin |
| --- | --- |
| 4 Hz | 1/1，每个 XVS 都接收 |
| 2 Hz | 1/2，每两个 XVS 接收一个 |

这样 cam0 和 cam1 才能任意组合 4/4、4/2、2/4、2/2，且修改一路不会改变另一路的共享时基。不能让 MCU 在 2 Hz 和 4 Hz 间按相机切换，因为两颗相机共用同一根 XVS。

### 6.4 稳定性、错误和资源

- 本轮短时连续采集 `sequence_drops=0`。
- 测试前有 1 次 `MIPI_CSI2 ERR2:0xf0000`；完成全部交互、UVC和服务重启测试后，本次开机累计8次。
- `vblank need >= 1000us, cur 696us` 本次开机累计38次，集中出现在启流/重启附近。
- 温度范围 43.461-44.384 C。
- USB共存补测结束后各温区约46.230-49.000 C。
- 1 分钟 load average 最大 0.16。
- `MemAvailable` 最低 3280568 kB。
- 没有专用 CPU/DDR/ISP 计数器数据。
- 没有功耗仪数据。
- 没有 30-60 分钟长稳。

因此阶段 3 是部分通过，2 Hz 短时输出通过，但 4 Hz、四组合、功耗和长稳未通过或未执行。

## 7. 阶段 4：驱动和 UART 控制

### 7.1 软件协议

本轮结果：

```text
CONTROL_UART_PROTOCOL_SELF_TEST_OK detail="cases=14"
XVS_PROTOCOL_SELF_TEST_OK
```

说明统一程序的软件命令解析、CRC 和同步协议格式能工作。相机参数命令均带 `camera_id`，程序通过 V4L2/RKAIQ/输出服务控制，不直接从应用层裸写 sensor 寄存器。

### 7.2 真实 UART 线

`/dev/ttyS9` 已配置为 115200、8N1、raw、无流控。发送：

```text
$XVS,1,PING*2D89\r\n
$XVS,1,STATUS*CEDA\r\n
```

计数从 `tx:0 rx:0` 变为 `tx:38 rx:0`，被动监听和主动等待都收到 0 字节。

这证明 RK3576 的发送路径工作，但不能证明板端 UART 双向通信。优先逐项检查：

1. CN4 pin19 `UART_CAM1_TX` 是否接 STM32 RX。
2. CN4 pin17 `UART_CAM1_RX` 是否接 STM32 TX。
3. 两端是否共地。
4. STM32 固件是否真的是 115200、8N1。
5. STM32 是否实现本文档规定的 `$ACK/$NACK` 协议。
6. RK3576 UART9 为 1.8 V，STM32 常为 3.3 V，必须检查双向电平转换和方向。
7. 用示波器先看 STM32 RX 上是否出现 RK3576 的 115200 波形，再看 STM32 TX 是否有应答。

阶段 4 判定为部分通过：应用层和 RK3576 TX 通过，真实 MCU RX/TX 闭环不通过。

## 8. 阶段 5：PPS、NMEA、Trigger 和同步

### 8.1 已验证的真实硬件部分

两颗 IMX586 均已启用 XVS Slave，连续帧间隔约 500 ms，证明 STM32 的固定 2 Hz XVS 已到达两颗传感器。

这只能证明两路跟随同一 XVS 节拍，不能仅靠软件证明曝光起点误差。V4L2 时间戳还包含 sensor 读出、MIPI、ISP 和驱动调度延迟。

### 8.2 软件模拟绑定

本轮执行：

```text
time-sync-reset
sync-bind-reset 1
sync-bind-log /root/camera_uart/test_results/20260810_v02_requirement_retest/sync_bind.csv
sync-sim-start 2 12
wait 8000
sync-sim-status
sync-bind-status
sync-bind-last
time-sync-status
```

产生 12 个模拟 trigger，CSV 得到 trigger_id 2-12 的 11 条完整绑定记录，其中照片保存阶段使用了 7 对。结果：

- 左右帧完成时间差 13-15 us；
- `trigger_id_gaps=0`；
- `duplicate_triggers=0`；
- `pending_overflows=0`。

模拟 trigger 和真实 2 Hz XVS 没有相位联系，`trigger_to_frame` 延迟逐帧增长，反而明确证明这些记录不能冒充真实 XVS 边沿。

### 8.3 还缺什么

当前 `TIME_SYNC_STATUS state=UNLOCKED utc_valid=0`。必须补齐：

- STM32 捕获真实 PPS 边沿；
- GPRMC/GNRMC 有效 UTC 秒；
- MCU 用同一硬件计数器锁存 PPS 和 XVS `timer_tick`；
- MCU 每个 XVS 上报递增 `trigger_id`；
- RK3576 状态达到 `UTC_LOCKED utc_valid=1`；
- 示波器或逻辑分析仪测量真实曝光起点同步误差。

阶段 5 判部分通过：真实 2 Hz XVS 跟随和软件绑定算法通过，完整 PPS/NMEA/Trigger/UTC 链没有通过。

## 9. 阶段 6：JPEG、EXIF 和帧元数据

### 9.1 保存结果

cam0 和 cam1 各保存 7 张 4000x3000 JPEG：

- `invalid_metadata=0`
- `encode_errors=0`
- `exif_errors=0`
- `write_errors=0`

CSV 包含 `camera_id`、`frame_id`、`trigger_id`、`trigger_source`、trigger 时间、`pps_id`、`utc_valid`、曝光、增益、ISO 和 JPEG 路径。

### 9.2 实际读取 JPEG，而不是只看 CSV

cam0 的一张实际 JPEG：

```text
4000x3000
ExposureTime=9998 us
PhotographicSensitivity=150
UserComment: camera_id=0;frame_id=76;trigger_id=2;gain_x1000=3000;iso=150;iso_estimated=1;utc_valid=0;trigger_source=SIM
```

cam1 对应 JPEG：

```text
4000x3000
ExposureTime=19996 us
PhotographicSensitivity=500
UserComment: camera_id=1;frame_id=23;trigger_id=2;gain_x1000=10000;iso=500;iso_estimated=1;utc_valid=0;trigger_source=SIM
```

两张照片使用相同模拟 `trigger_id=2` 和相同 `trigger_realtime_ns`，说明照片和模拟帧对的关联逻辑正确。

但 `DateTimeOriginal=2026:06:05` 是错误系统时间，`utc_valid=0`，`exposure_start_realtime_ns` 也是按模拟 trigger 推导的，不是真实传感器曝光起点测量。阶段 6 判部分通过。

## 10. 阶段 7：eMMC、网口和 UVC

### 10.1 eMMC 短保存

```text
save-start 0 /root/camera_uart/test_results/20260810_v02_requirement_retest/cam0_nv12
save-start 1 /root/camera_uart/test_results/20260810_v02_requirement_retest/cam1_nv12
wait 1300
save-stop 0
save-stop 1
capture-status all
```

两路各保存一帧 NV12，每帧 18,000,000 字节，`save_queue_drops=0`、`save_failures=0`。eMMC 短保存通过。

### 10.2 双路 HTTP 网口后端

程序在 `0.0.0.0:8080` 监听。板内分别取流 5 秒：

| 地址 | HTTP | 数据量 | MJPEG 帧边界 | 判定 |
| --- | --- | ---: | ---: | --- |
| `/cam0` | 200，MJPEG | 2,997,277 字节 | 5 | 通过 |
| `/cam1` | 200，MJPEG | 3,467,649 字节 | 5 | 通过 |

`curl` 最终显示超时是因为 MJPEG 本身是无限流，本测试故意在 5 秒停止；已经收到数 MB 且包含合法 JPEG `ff d8`，所以不是失败。

### 10.3 USB枚举和链路速度

软件配置中同时存在：

```text
rndis.0
uvc.0
uvc.1
```

接入电脑后实际状态为：

```text
UDC state: configured
current_speed: high-speed
maximum_speed: super-speed
usb0: UP 192.168.55.1/24
```

电脑`lsusb -t`同样显示复合设备位于480M链路，包含4个Video接口以及RNDIS/CDC Data接口。`maximum_speed=super-speed`只说明控制器能力上限，不能代表本次连接速度。因此复合USB枚举通过，USB3.0不通过。

### 10.4 双UVC实流

电脑端图像节点映射：

```text
/dev/video0 = cam0 Video Capture
/dev/video1 = cam0 UVC Metadata
/dev/video2 = cam1 Video Capture
/dev/video3 = cam1 UVC Metadata
```

两路都声明4000x3000 MJPEG的2 fps和4 fps。本轮做了三组实流测试：

| 工况 | cam0 | cam1 | 判定 |
| --- | --- | --- | --- |
| 初始双路20帧 | 4000x3000，20帧可解码 | 4000x3000，20帧可解码 | 双端点通过，但当时约1 fps |
| 修正input-thin后的并发12帧 | 12帧可解码，稳态约500 ms/帧 | 12帧可解码，稳态约500 ms/帧 | 两路真实约2 fps |
| 恢复常驻服务后的并发8帧 | 8帧可解码，稳态约500 ms/帧 | 8帧可解码，稳态约500 ms/帧 | 最终运行状态通过 |

cam0和cam1的抽帧图分别表现为普通视角和鱼眼视角，证明两个UVC图像端点不是同一路画面复制。证据位于`usb_connected/cam0_uvc.jpg`、`cam1_uvc.jpg`以及对应MJPEG文件。

UVC刚打开时序号从0跳到3，报告`dropped: 2`；一次并发压力测试中，开头还出现若干2036字节带error标志的占位帧，约3秒后恢复为完整JPEG。稳态帧连续且文件可解码，但启动瞬态仍应优化，不能判定为全程零丢帧。

### 10.5 RNDIS、SSH、双HTTP及共存

- 电脑RNDIS接口为`enxfebb20a2ee76`，地址`192.168.55.13/24`；板端`usb0=192.168.55.1/24`。
- 多轮ping均为0%丢包；最终6次ping平均约0.249 ms。
- 已通过`ssh root@192.168.55.1`登录，常驻服务状态为`active`。
- 双HTTP并发8秒，cam0收到约4.81 MB、cam1约5.45 MB，均解析为8张4000x3000 JPEG。
- 双UVC、双HTTP和RNDIS同时运行时，双UVC各取8帧、双HTTP各取10秒并解析为10帧，同时12次ping零丢包。
- input-thin修正后的再次共存测试中，双UVC各解析12帧，双HTTP各解析16帧，ping仍零丢包。

因此，当前USB2.0带宽下双UVC、双HTTP、RNDIS可以同时工作；这只覆盖当前约2 fps场景，不代表未来双路4 fps在USB2.0下也一定有带宽余量。

### 10.6 停止UVC时保持网口在线

交互程序执行：

```text
uvc-stop all
OK command=uvc-stop target=all usb_gadget=kept rndis=kept
```

软停止后实测：复合USB仍枚举、四个UVC控制/元数据节点仍存在、RNDIS接口仍存在、6次ping零丢包、SSH登录成功、cam0 HTTP 4秒收到约2.36 MB。随后`uvc-start all`也能重新启用两路UVC。因此“停UVC送帧但保留网口”通过。

注意不能用`systemctl stop camera-uvc.service`代替`uvc-stop all`。停止整个服务会退出进程并撤下整个复合Gadget，UVC和RNDIS都会消失。这是服务级停止的预期生命周期，不符合“只停相机、网口保持”的产品操作语义。

### 10.7 阶段7判定

eMMC、双UVC、双HTTP、RNDIS ping/SSH、三者共存以及UVC软停止时保留网口均通过。USB3.0链路和UVC启动瞬态未通过，所以阶段7整体仍判部分通过。

## 11. 已知告警如何理解

### 11.1 MIPI 和 vblank

本轮新增的 `MIPI_CSI2 ERR2` 和 `vblank cur 696us` 都出现在流重启附近。短时帧序列没有跳号，所以不能直接说正在持续丢帧，但也不能忽略。应修正 sensor mode 的垂直消隐到至少 1000 us，并在修正后重跑启停 100 次和 30-60 分钟长稳，要求错误计数不增加。

### 11.2 RKAIQ 退出告警

退出/重启时出现 `pool items are still in use`、`trigger_isp_readback buf not ready`。它们是资源释放时序告警，不等价于运行期间的 sequence drop，但产品版本仍应确保先停止输出队列、等待编码任务清空，再停止采集和销毁 AIQ context。

## 12. 下一轮应按这个顺序补测

1. 先修通 STM32 UART：必须收到 PONG 和 STATUS，确认实际频率和脉冲计数。
2. 让 MCU 固定输出 4 Hz XVS，依次验收 4/4、4/2、2/4、2/2，每种至少 60 秒。
3. 接 PPS 和 GNSS，看到 `UTC_LOCKED utc_valid=1` 后再保存照片。
4. 用真实 `EVT,XVS trigger_id/timer_tick` 完成 1000 次触发绑定，要求无 gap、duplicate、overflow。
5. 用示波器或逻辑分析仪验收 XVS 到两颗传感器输入以及曝光起点误差；只有一通道时只能分次测同一参考信号，不能证明两路瞬时相对误差。
6. 将RK3576到电脑的整条Type-C/Hub链路切换到SuperSpeed，并用`lsusb -t`和板端`current_speed`同时确认5000M/super-speed；随后重测双UVC 4 fps共存。
7. 清理 eMMC 空间后做 30-60 分钟长稳，并接功耗仪记录待机、单路、双路、UVC、HTTP、eMMC 工况。
8. 获取广角和鱼眼各自 IQ 标定，分别验证 AWB、BLC、LSC、LDC 不串扰。

## 13. 测试结束状态

测试程序正常退出，批处理返回 `APP_RC=0`。常驻服务已恢复：

```text
camera-uvc.service = active
MainPID = 6750
NRestarts = 0
Process = /root/camera_uart/camera_aiq_test --all-daemon
```

UDC最终为`configured`，`usb0`为`UP 192.168.55.1/24`，电脑端最终可ping、可SSH、双HTTP可访问、双UVC节点存在并可取流。实际USB速度仍为`high-speed/480M`。本轮没有删除或覆盖此前测试结果。
