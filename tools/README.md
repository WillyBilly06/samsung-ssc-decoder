# Tools

| Script | What it does |
|---|---|
| `build_pure_sink.ps1` | Builds the speaker firmware with the bundled ESP-IDF (ccache on) and checks the image size |
| `flash_pure_sink.ps1 [-Port COM7]` | Flashes bootloader, partition table and app; keeps NVS (pairings) |
| `serial_log.py PORT SECONDS [out.log] [--no-reset] [--baud=N]` | Captures the console (default 921600 baud) |

`serial_log.py` resets the board first, so the log starts at boot. With `--no-reset` it attaches to a running board
without toggling DTR/RTS. It needs `pip install pyserial`.
