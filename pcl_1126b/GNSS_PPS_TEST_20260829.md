# RV1126B direct GNSS/PPS test

This build uses PPS only to calibrate RV1126B timestamps. It does not trigger
the IMX586 and does not step the Linux system clock.

## Wiring

- GNSS TX (GPRMC/GNRMC) -> CN5 pin 17 (`UART_CAM1_RX`, `/dev/ttyS2`)
- GNSS PPS -> CN5 pin 19 (`UART_CAM1_TX`, repurposed as GPIO3_B1 PPS input)
- GNSS GND -> camera board GND

The UART/PPS bank is 3.3 V. Do not apply 5 V. The default configuration expects
a rising-edge PPS and 115200 8N1 NMEA input. CN5 pin 23 is not a PPS input to
RV1126B; it only reaches the camera timing circuit.

XDAS camera control moved to UART1 M1 (`/dev/ttyS1`), using the
`TPUART_RX1`/`TPUART_TX1` test points.

## Board test

First verify the kernel devices:

```sh
ls -l /dev/pps0 /dev/ttyS1 /dev/ttyS2
cat /sys/class/pps/pps0/name
```

Stop the camera service so it does not own the GNSS UART, then monitor without
opening either camera:

```sh
/etc/init.d/S60pcl-camera stop
/usr/bin/camera_aiq_test --gnss-monitor \
  --gnss-uart /dev/ttyS2 --gnss-baud 115200 \
  --pps-device /dev/pps0 --gnss-rmc-delay-ms 900
```

After GNSS obtains a valid fix, expect:

- `uart_connected=1`, `pps_connected=1`
- `pps_events` and `rmc_events` increasing once per second
- `state=UTC_LOCKED`, `utc_valid=1`
- `invalid_rmc=0`, `unpaired_rmc=0`

Use Ctrl-C to stop the monitor. Enable autostart only after the direct GNSS test
passes. The normal camera process receives the same four GNSS options through
`/etc/pcl-camera.conf`; GNSS-calibrated UTC is written to JPEG EXIF and metadata
for frames whose V4L2 timestamp is monotonic.
