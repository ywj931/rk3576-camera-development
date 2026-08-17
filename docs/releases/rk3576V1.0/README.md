# rk3576V1.0版本

这是 2026-08-17 完成实机复测后的 RK3576 双 IMX586 源码基线。

程序源码位于：

- `app/camera_uart`：统一控制、双路采集、RKAIQ、UART、同步绑定、EXIF、UVC、HTTP 和 eMMC。
- `kernel-6.1/drivers/media/i2c/imx586.c`：IMX586 XVS slave 支持。
- `kernel-6.1/arch/arm64/boot/dts/rockchip`：双相机和 Type-C 配置。
- `external/uvc_app`、`external/rkscript`：双 UVC/RNDIS Gadget 链路。

实测 AArch64 程序 SHA-256：

```text
c514571b29cb68b31486ee85eb0732067793c6360a9755276e869d7bd0c1a7a3
```

主要实测结果：双路 4000x3000、独立曝光/增益/ISO、固定外部 2 Hz、
60 秒无丢帧、PB8 白灯光学同步、eMMC、双 UVC 和 RNDIS/HTTP 均通过。

完整 v0.2 仍为部分通过。MCU UART、PPS/NMEA/UTC、真实 MCU trigger_id
绑定、4 Hz、示波器精度、镜头独立 IQ、USB3、整机功耗和长稳尚未验收。

- [完整复测报告](RK3576_IMX586_V02_LED_SYNC_FULL_RETEST_REPORT_20260817.md)
- [阶段 5 光学证据](stage5_evidence/sync_seq75_evidence.jpg)
- [阶段 5 逐对统计](stage5_evidence/stage5_pair_analysis.csv)
- [综合统计摘要](retest_summary.txt)

本版本提交源码、配置、报告和精简证据；不提交原始 NV12、构建中间文件、
系统镜像或完整测试视频。
