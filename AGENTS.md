# Repository Guidelines

## Project Structure & Module Organization
This repository contains firmware for the M5Stack StickS3 hardware buddy. Core application code lives in `src/`: `main.cpp` drives the UI and state machine, `ble_bridge.cpp` handles BLE transport, `character.cpp` renders GIF pets, and `buddy.cpp` plus `src/buddies/*.cpp` implement ASCII pets. Example character packs live in `characters/`, board-specific settings in `boards/`, reference docs in `README.md` and `REFERENCE.md`, and helper scripts in `tools/`.

## Build, Test, and Development Commands
Use PlatformIO Core from the repo root.

- `pio run -e m5stack-sticks3` builds the firmware for the StickS3 target.
- `pio run -e m5stack-sticks3 -t upload` flashes firmware over USB.
- `pio run -e m5stack-sticks3 -t erase` wipes flash before a clean upload.
- `pio run -e m5stack-sticks3 -t uploadfs` uploads the LittleFS image used for character assets.
- `pio device monitor -b 115200` opens the serial console.
- `python3 tools/flash_character.py characters/bufo` stages and uploads a sample GIF character pack.

## Coding Style & Naming Conventions
Follow the existing C++ style in `src/`: 2-space indentation, opening braces on the same line, `camelCase` for functions, and `UPPER_SNAKE_CASE` for macros and fixed constants. Keep files focused by feature area; add new buddy animations as one file per species under `src/buddies/`. Prefer short, targeted comments explaining hardware or protocol constraints rather than obvious control flow. No formatter or linter is configured here, so match surrounding code closely.

## Testing Guidelines
There is no automated unit-test suite or coverage gate in this repo. Validate changes by building with `pio run -e m5stack-sticks3`, then test on hardware over serial and BLE. The scripts `tools/test_serial.py` and `tools/test_xfer.py` are useful for transport-level checks when modifying the protocol or file transfer path. If you touch character upload behavior, verify both BLE folder push and `uploadfs`.

## Commit & Pull Request Guidelines
Recent history uses short, imperative commit subjects, sometimes with a PR suffix, for example: `Add screenshots to README and bridge-enable steps to REFERENCE (#1)`. Keep commits focused and descriptive. PRs should explain the hardware-visible impact, list the board and commands used for validation, and include photos or screenshots when changing UI, pairing flow, or character rendering.

## Contribution Scope
`CONTRIBUTING.md` sets a narrow bar: fixes that keep the reference implementation working are welcome, but new features, board ports, and broad refactors are usually out of scope. If you want to substantially change behavior, fork the project instead of expanding this reference firmware.
