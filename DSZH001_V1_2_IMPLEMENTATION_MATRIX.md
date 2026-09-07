# DSZH001 v1.2 Implementation Matrix

Source of truth: `接口控制协议v1.2.pdf` dated 2026-04-15.  This matrix is
limited to the commands and messages in that document; `01 19` shutter and
`01 1A` ISO are retained as local extensions and are not v1.2 commands.

| Command | Meaning | Status | Runtime behavior / boundary |
| --- | --- | --- | --- |
| `00 00` | Get camera version | Implemented | Returns configured version as type `0x02` string. |
| `00 02` | Get camera mode | Partially implemented | Returns configured UVC/UDISK mode value. Only UVC can currently be entered, so it cannot report a successfully-entered UDISK mode. |
| `00 03` | Get remaining/total capacity | Implemented | Calls `statvfs` for the configured local-save root; filesystem failure is NACK (`0x02`). |
| `00 04` | Get camera time | Implemented | Uses GNSS-resolved time when valid, otherwise `CLOCK_REALTIME`. |
| `01 04` | Reboot device | Partially implemented | ACK is emitted after queueing a delayed `sync()`/`reboot()`. A later kernel reboot failure cannot be reported over the disconnected UART. |
| `01 11` | Start local save | Implemented | Starts the real JPEG backend for every selected camera; failure rolls back and NACKs. |
| `01 12` | Stop local save | Implemented | Stops the real JPEG backend and reports a saved backend error as NACK. |
| `01 14` | Set UDISK mode | Cannot safely implement | No proven upper-layer UDISK gadget/lifecycle backend exists. It remains `0xff` (unsupported); it is not interpreted as UVC stop. |
| `01 15` | Set UVC mode | Implemented | Starts the real UVC backend after capture-state validation; start failure NACKs. v1.2 defines no UVC-close command. |
| `01 16` | Set camera time | Implemented | Strictly accepts `SSSSSSSSSS.ffffff`, rejects time changes during save, and ACKs only after `clock_settime` succeeds. It requires `CAP_SYS_TIME`. |
| `01 17` | Set maximum exposure | Implemented | Accepts slots `0..8` (`1/20` through `1/800`), maps to `50000..1250 us`, pre-reads both cameras, calls RKAIQ `setExpSwAttr` for each configured camera, and rolls back already-applied cameras if a later camera fails. Any backend failure NACKs. |
| `01 18` | Set PPS frequency | Cannot safely implement | The application has no verified PPS-output frequency actuator. Existing XVS control has a distinct 4 Hz safety contract, so this command remains `0xff` rather than retiming it. |

All handled frames use CRC-8/0xD5. Bad header, length, CRC, parameter, and
backend failures produce the protocol error responses; a successful ACK is not
sent before its synchronous backend call succeeds.

The document's six `40 00` upgrade/device lifecycle messages are unsolicited
status messages, not host commands. This camera application does not own the
firmware upgrade lifecycle and therefore does not emit them.
