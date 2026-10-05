# Pulsedart firmware sources

| Folder / file | What |
|---|---|
| `boards/hyperx/pulsedart/` | Zephyr board definition: pins (see `../PINMAP.md`), partitions, HID nodes |
| `mouse/` | the firmware (`src/`, `prj.conf`, `Kconfig`; MCUboot variant files in `mcuboot/`) |
| `mouse/local.conf` | optional personal Kconfig fragment, not in git (e.g. your dongle record) |
| `app/` | stage-1 bring-up test: cycles LED colours, logs buttons over RTT |
| `build.ps1` | `.\build.ps1 mouse`: build with NCS v3.4.1 from `C:\ncs` |
| `build-mcuboot.ps1` | MCUboot + signed image variant (not used yet) |
| `keys/` | MCUboot signing key, private, not in git |
| `TESTPLAN.md` | the original first-run test plan (historical) |

Before the first build, generate the stock-derived data (`mouse/src/stock_blobs.c`) from
your own dump: `python tools/extract_blobs.py dump/flash.bin`. `build-release.ps1` builds a
release with marker bytes instead; `tools/make_firmware.py` completes it without a compiler.

Documentation:
- [../docs/firmware.md](../docs/firmware.md): what the firmware does;
- [../docs/flashing.md](../docs/flashing.md): build, flash, restore;
- [../docs/testing.md](../docs/testing.md): what was verified.
