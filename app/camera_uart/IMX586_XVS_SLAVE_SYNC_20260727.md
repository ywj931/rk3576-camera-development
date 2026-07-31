# RK3576 双 IMX586 XVS 从模式修改与验证

日期：2026-07-27

## 1. 本次实现范围

本次实现的是两颗 IMX586 的“传感器侧 XVS 从模式”：

- cam0、cam1 都把 XVS 配置为 1.8 V、低电平有效的输入。
- 两颗 sensor 都禁止 XVS 输出，避免共线冲突。
- sensor 进入 streaming 后等待外部 XVS；没有 XVS 脉冲时不应持续出帧。
- 曝光和模拟增益写入增加 Group Hold。
- 曝光改变时同步更新 `PRSH_LENGTH_LINES = COARSE_INTEG_TIME + 34`。

本次没有实现：

- RK3576 产生 FSYNC_CAM/XVS 脉冲的 GPIO、PWM 或定时器程序。
- trigger 中断时间戳和 cam0/cam1 frame_id 绑定。
- UART 同步开关协议。

所以当前完成的是硬件同步链路的 sensor 端，下一步必须实现或接入外部 XVS 脉冲源。

## 2. 修改文件

驱动：

`kernel-6.1/drivers/media/i2c/imx586.c`

设备树：

`kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi0.dtsi`

`kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-3m-rk3576-csi1.dtsi`

两路节点都增加：

```dts
sony,xvs-slave-mode;
```

驱动只在设备树存在该布尔属性时启用 XVS 从模式。删除该属性并重新编译，即可恢复原来的自由运行模式。

## 3. 驱动增加的寄存器

| 地址 | 写入值 | 作用 |
|---|---:|---|
| `0x3F70` | `1` | `MC_MODE` 固定预快门时间 |
| `0x3041` | `0` | Slave mode |
| `0x3040` | `0` | XVS 配置为输入 |
| `0x3F71` | `0` | 禁止 sensor 输出 XVS |
| `0x4B85` | `1` | XVS 输入低电平有效 |
| `0x0350` | `1` | 帧长自动跟随曝光 |
| `0x3F68~0x3F69` | `0` | XVS 行延迟为 0 |
| `0x3F6A~0x3F6B` | `0` | XVS 像素时钟延迟为 0 |
| `0x3F6E` | `0` | 不使用倍频模式 |
| `0x3F6F` | `0` | 不抽帧 |
| `0x3F79~0x3F7B` | 曝光行数 + 34 | 固定预快门长度 |
| `0x0104` | `1/0` | Group Hold 开始/提交 |

这些同步模式寄存器在 `MODE_SELECT=1` 之前、sensor 仍处于 standby 时写入。

## 4. 为什么曝光必须联动 PRSH

IMX586 手册定义：

```text
PRSH_LENGTH_LINES >= COARSE_INTEG_TIME + 34
```

`0x0202/0x0203` 的 `COARSE_INTEG_TIME` 是 16 位无符号整数，单位为行。驱动原来“低 4 位是小数”的注释与当前 IMX586 手册不符，已移除。

如果只修改曝光而不更新 PRSH，较长曝光可能超过预快门窗口，外部同步首帧会不稳定或不出帧。因此驱动每次设置曝光时，在同一个 Group Hold 流程内同时写曝光和 PRSH。

模拟增益仍写 `0x0204/0x0205`，没有改成数字增益。XVS slave streaming 时，手册要求增益修改使用 Group Hold，本次已经实现。

## 5. 硬件要求

根据现有原理图，`FSYNC_CAM` 经 1.8 V 缓冲后连接到两颗相机的公共 XVS。必须满足：

- 两颗 IMX586 都工作在 XVS 输入模式。
- XVS 空闲电平为高，低脉冲有效。
- 两路必须由同一个硬件脉冲源驱动，不能用两个普通用户态线程分别翻转 GPIO。
- 电平必须是 1.8 V，不可直接输入 3.3 V。
- 上电和切换阶段不能让任一 sensor 驱动公共 XVS 线。

手册给出的最小低脉宽是 8 个 VTPXCK。当前模式 VTPXCK 约 103.2 MHz，理论最小值约 77.5 ns。普通 Linux 用户态 GPIO 无法稳定产生该量级的脉冲，实际验证建议由 PWM、硬件定时器、MCU 或 FPGA 产生 5~10 us 的低脉冲。

目标 2 Hz 时周期为 500 ms，目标 4 Hz 时周期为 250 ms。

## 6. 构建结果

执行：

```sh
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux
./build.sh kernel
```

构建已通过：

- `drivers/media/i2c/imx586.o` 编译成功。
- `tspi-3m-rk3576.dtb` 编译成功。
- `vmlinux`、`Image`、`resource.img`、`boot.img` 生成成功。
- 内核补丁检查结果：0 errors，0 warnings。
- 反编译最终 DTB 后，module-index 0 和 1 两个 camera 节点均存在 `sony,xvs-slave-mode`。

最终镜像：

`/home/ywj/rk3576_sdk/TaishanPi-3-Linux/output/firmware/boot.img`

## 7. 上板验证顺序

### 7.1 刷入并确认驱动

通过当前板卡已有的刷机流程写入新的 `boot.img`，重启后检查：

```sh
dmesg | grep -E "imx586|XVS slave"
```

两路开始 streaming 时都应看到类似：

```text
XVS slave mode enabled: low-active, exposure=2816, prsh=2850 lines
```

如果出现 `failed to configure XVS slave mode`，先检查 I2C、sensor 供电和寄存器写入。

### 7.2 无脉冲等待测试

先保持 XVS 高电平，不发送脉冲，再启动现有双路程序：

```sh
./camera_aiq_test
stream-start all
```

预期两路进入等待 XVS 状态，不应按原帧率连续收到图像。这不是故障，而是从模式的正确行为。

### 7.3 单颗外触发

第一次只连接/启用 cam0：

1. 启动 cam0 stream。
2. XVS 保持高电平。
3. 发送第一个低脉冲，进入 pre-shutter。
4. 等待至少 `(PRSH_LENGTH_LINES + 2) * Tline`。
5. 发送第二个低脉冲，第一帧才会输出。
6. 后续每个脉冲输出一帧。

默认曝光 2816 行、PRSH 2850 行、Tline 约 10.87 us，第一和第二个脉冲间隔至少约 31 ms。2 Hz 或 4 Hz 的正常周期已经大于该值。

### 7.4 双颗共触发

单颗成功后，再让同一个 XVS 信号同时驱动 cam0、cam1：

```text
stream-start all
sync-status
capture-status all
```

验收重点：

- 每个 XVS 周期两路各增加一帧。
- 两路长期 frame count 差值不增长。
- `sequence_drops=0`。
- `sync-status` 的 `delta_ns` 相比软件自由运行明显收敛。
- 双路保存时 `save_queue_drops=0`、`save_failures=0`。

`sync-status` 当前仍是 ISP/V4L2 时间戳诊断，不能单独证明 sensor 曝光起点完全同步。最终硬件同步验收需要示波器观察公共 XVS，并增加 trigger 时间戳到双路 frame_id 的绑定。

## 8. 重要限制

启用 `sony,xvs-slave-mode` 后，如果外部 XVS 没有工作，两个摄像头会等待而没有画面。调试其他功能时若仍需要自由运行，先从 cam0、cam1 设备树删除该属性并重新构建 `boot.img`。

当前 `RKMODULE_SET_QUICK_STREAM` 只切换 `0x0100`。它假设此前已经完成一次正常的 full stream 初始化。首次启动和断电重启后，必须走标准 `stream-start`，不要直接依赖 quick stream 打开同步模式。

## 9. 备份与恢复

原文件备份目录：

`/home/ywj/rk3576_sdk/TaishanPi-3-Linux/backups/imx586_xvs_20260727/`

其中包含：

- 原始 `imx586.c`
- 原始 `tspi-3m-rk3576-csi0.dtsi`
- 原始 `tspi-3m-rk3576-csi1.dtsi`
- 最终差异 `imx586_xvs_slave.patch`

恢复时优先根据补丁审阅后反向应用，或从备份目录逐个恢复对应文件，再执行 `./build.sh kernel`。

