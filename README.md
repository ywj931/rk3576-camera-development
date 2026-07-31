# RK3576 dual IMX586 camera development backup

This repository stores the source changes made in the local Rockchip
`TaishanPi-3-Linux` repo workspace. It is an overlay, not a complete 48 GB SDK
mirror.

## Included

- `app/camera_uart`: UART control, RKAIQ control, dual capture, network/UVC
  output, XVS control, trigger/frame binding, EXIF support, tests and documents.
- `kernel-6.1`: IMX586 XVS slave mode, RK3576 camera/Type-C DTS changes and UVC
  queue changes.
- `external/camera_engine_rkaiq`: dual-camera RKAIQ changes.
- `external/uvc_app` and `external/rkscript`: UVC gadget and USB mode changes.
- `device/rockchip`: Ubuntu rootfs build-script changes.
- `docs/cn/RK3576/Camera`: camera and UART validation documents.
- `BASE_MANIFEST.xml`: pinned upstream repo revisions for reconstructing the SDK
  baseline.

Generated firmware, root filesystems, compiler outputs, test videos, captured
images, logs, historical `.orig` files and local backup directories are not
included.

## Restore

1. Initialize and sync a Rockchip SDK workspace using `BASE_MANIFEST.xml` or the
   matching vendor manifest.
2. Review the target SDK revision before applying the overlay.
3. Copy this repository's `app`, `device`, `docs`, `external` and `kernel-6.1`
   directories over the SDK root while preserving paths.
4. Rebuild the application and affected kernel/boot images, then run the
   validation procedures under `app/camera_uart`.

The overlay contains complete current versions of changed files. It intentionally
does not include each upstream repository's full history.
