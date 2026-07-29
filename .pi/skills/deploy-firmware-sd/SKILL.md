---
name: deploy-firmware-sd
description: "Build CrossPoint firmware and upload firmware.bin to the SD card root over the device web server. Use when the user wants to push a new build to a device by IP for on-device flashing."
---

# Deploy Firmware to SD

The device web server (`src/network/CrossPointWebServer.cpp`) exposes:
- `POST /delete` — form arg `path` (or JSON `paths`)
- `POST /upload?path=/` — multipart file; **rejects the request if the file already exists**

## Workflow

1. Ask the user for the device IP if not given.
2. Build:

   ```sh
   pio run
   ```

3. Upload (script deletes `/firmware.bin` first, then uploads):

   ```sh
   .pi/skills/deploy-firmware-sd/upload-firmware.sh <device-ip> [path/to/firmware.bin]
   ```

   Default binary: `.pio/build/default/firmware.bin`. For other envs pass the path explicitly
   (e.g. `pio run -e gh_release` → `.pio/build/gh_release/firmware.bin`).

Success prints `File uploaded successfully: firmware.bin`.
