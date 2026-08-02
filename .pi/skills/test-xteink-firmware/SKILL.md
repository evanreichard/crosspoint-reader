---
name: test-xteink-firmware
description: "Build and test CrossPoint firmware in the native Xteink emulator. Use for UI flows, buttons, screenshots, serial assertions, SD behavior, or networking."
---

# Test CrossPoint Firmware

Build the firmware, then drive it with `esp-emu --board xteink`. Prefer observable firmware logs over sleeps or guessed render times. If a meaningful state has no stable log, add a concise semantic firmware log rather than introducing a delay.

## Prerequisite

Before building or testing, verify the global tool is available:

```sh
command -v esp-emu
esp-emu --board xteink --help >/dev/null
```

Stop if either command fails. Tell the user to install `esp-emu` from [qemu-esp-boards](https://gitea.va.reichard.io/evan/qemu-esp-boards.git) (`nix run .#esp-emu`, or `uv tool install -e tools/esp-emu-cli`) before continuing; installation is the user's responsibility.

## Build

```sh
pio run -e default
```

The firmware is `.pio/build/default/firmware.bin`. The emulator builds native QEMU on first boot and stores state under `/tmp/esp-emu-xteink-$UID` by default.

## Scripted Test

Use an executable `.xteink` script:

```text
#!/usr/bin/env -S esp-emu --board xteink

boot .pio/build/default/firmware.bin
wait-log "Entering activity: Home" --timeout 60

press bottom-4 bottom-4 bottom-4 bottom-4
press bottom-2
wait-log "Entering activity: Settings"

press bottom-2 bottom-2 bottom-2
wait-log "\[SETTINGS\] Category index: 3"

capture _scratch/emulator-settings-system.png
```

Save it, make it executable, and run it from the project root:

```sh
chmod +x test-settings.xteink
./test-settings.xteink
```

Each line is a normal CLI command. Blank lines and `#` comments are allowed. Scripts may also run explicitly or through stdin:

```sh
esp-emu --board xteink --state /tmp/my-test run flow.xteink
printf '%s\n' 'boot .pio/build/default/firmware.bin' 'wait-log "Entering activity: Home"' | esp-emu --board xteink run
```

## Synchronization

- Use `wait-log REGEX` as the default synchronization and assertion mechanism.
- `press` already waits for debounced press and release sampling; multiple presses do not need frame waits.
- Use `wait-frame [COUNT]` only when an e-ink redraw is itself the behavior under test.
- Use `wait-idle SECONDS` only when silence is the actual readiness signal.
- Capture and inspect the final screen after semantic log assertions.

## SD Cards

```sh
esp-emu --board xteink boot .pio/build/default/firmware.bin                         # persistent state image
esp-emu --board xteink boot .pio/build/default/firmware.bin --sdcard test-library/ # merge folder before boot
esp-emu --board xteink boot .pio/build/default/firmware.bin --sdcard card.img      # use image directly
```

Directory contents overwrite matching files in the persistent state image; other image contents remain and the source directory is unchanged. Add `--fresh-sd` when the test requires a clean card.

## Useful Commands

```sh
esp-emu --board xteink press bottom-2
esp-emu --board xteink hold power
esp-emu --board xteink release power
esp-emu --board xteink capture /tmp/screen.png
esp-emu --board xteink web
esp-emu --board xteink stop
```

Buttons are `left`, `right`, `bottom-1` through `bottom-4`, and `power`. Screen labels are the authoritative mapping. Serial output and assembled flash are `serial.log` and `flash.bin` inside the selected state directory.
