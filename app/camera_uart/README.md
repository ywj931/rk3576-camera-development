# RK3576 Camera UART Test

This is a standalone userspace test tool for the camera-control UART. It does
not depend on RKAIQ and does not select either camera or IQ process.

The current board mapping is:

- device: `/dev/ttyS9`
- controller: `2adc0000.serial`
- pin group: `uart9m1-xfer`
- RX: `GPIO3_B2` (`UART_CAM1_RX` on HOT_SHOE1)
- TX: `GPIO3_B3` (`UART_CAM1_TX` on HOT_SHOE1)
- line settings: 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control

Build on the RK3576 board:

```sh
make
```

Cross-build a deployable ARM64 executable on the SDK host:

```sh
make aarch64
```

Run the controller-internal loopback test (no external wire is required):

```sh
./camera_uart_test loop
```

Send one message through the external TX pin:

```sh
./camera_uart_test send RK3576_TX_TEST
```

Echo every byte received on the external RX pin back through TX:

```sh
./camera_uart_test echo
```

The internal loopback test does not verify HOT_SHOE1 voltage, soldering, cable,
or crossed TX/RX wiring. Use a separate 3.3 V USB-TTL adapter for the external
send/echo tests. Connect TX to RX, RX to TX, and GND to GND. Do not connect the
adapter VCC pin.
