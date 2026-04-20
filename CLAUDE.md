# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A reference ESP32/Arduino firmware for the M5StickC Plus that implements the Claude desktop apps' Hardware Buddy BLE protocol. The authoritative protocol spec is `REFERENCE.md` — this firmware is one worked example of honoring that contract.

Per `CONTRIBUTING.md`, the project is intentionally not actively maintained. Accepted changes are limited to: fixes to `REFERENCE.md` when the protocol docs are wrong, and bug fixes that make the reference "not work as a reference" (won't pair, won't render, crashes on boot). **Do not add features, new pets, new screens, board ports, refactors, or dependency bumps** unless the user explicitly asks — suggest forking instead.

## Build, flash, test

Toolchain is [PlatformIO](https://docs.platformio.org/en/latest/core/installation/). Single env `m5stickc-plus` in `platformio.ini`; framework=arduino, filesystem=littlefs, partitions=`no_ota.csv`, CPU 160MHz. `build_src_filter` pulls in `src/*` plus `src/buddies/*`.

```bash
pio run                       # compile
pio run -t upload             # flash firmware over USB
pio run -t uploadfs           # flash LittleFS (contents of ./data/)
pio run -t erase              # wipe flash; follow with upload for a clean boot
pio device monitor -b 115200  # serial console
```

There is no unit-test suite; the `tools/test_*.py` scripts are end-to-end smoke tests run against a connected stick over USB serial:

```bash
python3 tools/test_serial.py          # cycle heartbeat snapshots → observe state transitions
python3 tools/test_xfer.py            # exercise the folder-push receiver over serial
python3 tools/prep_character.py <dir> # normalize GIFs to 96px-wide cross-state crop
python3 tools/flash_character.py characters/bufo   # stage into data/ + pio run -t uploadfs
```

`data/` is gitignored — `flash_character.py` populates `data/characters/<name>/` and invokes `uploadfs`. Character packs must total <1.8MB.

## Architecture

The firmware is a single-loop state machine driven by BLE JSON. Understanding the data flow across these files is the fastest way in:

- **`src/main.cpp`** — `setup()`/`loop()`, persona state machine (`PersonaState`: sleep/idle/busy/attention/celebrate/dizzy/heart), UI screens (normal / pet / info / approval / menu), button handling, screen power, shake/face-down detection, LED.
- **`src/ble_bridge.cpp` + `.h`** — NimBLE Nordic UART Service (UUIDs in `REFERENCE.md`). Advertises `Claude-XXXX` (last 2 MAC bytes). Handles line-buffered TX (device→desktop notify) and RX (desktop→device write), reassembling MTU-fragmented `\n`-terminated JSON. Also owns LE Secure Connections bonding.
- **`src/data.h`** — parses the heartbeat snapshot, turn events, `time`/`owner`/`name`/`unpair`/`status` commands; derives the persona state from `total`/`running`/`waiting`/`prompt`.
- **`src/xfer.h`** — folder-push receiver state machine (`char_begin` → repeated `file`/`chunk`/`file_end` → `char_end`). Writes through LittleFS into `/characters/<name>/`. Validates `file.path` (rejects `..` and absolute paths).
- **`src/character.cpp` + `.h`** — GIF mode. Reads `manifest.json` + state GIFs via `AnimatedGIF`, renders into the shared `TFT_eSprite`. Honors manifest `colors` and arrays-of-GIFs for rotating idle animations.
- **`src/buddy.cpp` + `src/buddies/*.cpp`** — ASCII species dispatch. Each species file exposes 7 `StateFn`s indexed by `PersonaState` (see `Species` in `src/buddy.h`). Adding a species requires registering it in the species table in `buddy.cpp`; `build_src_filter` in `platformio.ini` already includes `buddies/`.
- **`src/stats.h`** — NVS-backed counters (approvals, denies, tokens, level, naps), settings (brightness, species index), owner/device name. Token level-up thresholds drive the `celebrate` one-shot.

### Species / GIF mode interplay

`speciesIdx == 0xFF` (`SPECIES_GIF` in `main.cpp`) means "render the installed GIF pack." `nextPet()` cycles GIF → species 0 → species 1 → … → last species → GIF (only if a pack is installed; otherwise the GIF slot is skipped). `characterInvalidate()` must be called whenever the pet changes so the renderer drops cached frames.

### Persona state selection

`PersonaState` is derived from the heartbeat: `waiting > 0` → attention (LED blinks), `running > 0` → busy, connected + idle → idle, disconnected → sleep. `celebrate`/`dizzy`/`heart` are one-shots overlaid via `oneShotUntil`: token-threshold crossings trigger celebrate, IMU shake triggers dizzy, approving a prompt in <5s triggers heart. The approval screen is its own UI mode and takes over the A/B buttons while a `prompt` is present — preserve that mapping (A=approve, B=deny) when touching button handling.

### Wire-protocol invariants to preserve

Any edit under `src/ble_bridge.*`, `src/data.h`, or `src/xfer.h` is touching the protocol surface. Cross-check against `REFERENCE.md`:

- One UTF-8 JSON object per line, `\n`-terminated; reassemble across notifications.
- Every `cmd:` from the desktop requires a matching `{"ack":"<cmd>","ok":...}` — including `char_begin`/`file`/`chunk`/`file_end`/`char_end`. Not acking `char_begin` is how a device opts out of folder push.
- Permission reply must echo `prompt.id` exactly.
- Include `"sec": true` in the `status` ack `data` once the link is encrypted; handle `{"cmd":"unpair"}` by erasing stored bonds.
- 4KB cap on turn events is enforced on the desktop side; the device just consumes them.
