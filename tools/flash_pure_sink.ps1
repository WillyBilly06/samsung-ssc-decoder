# Flashes the SSC sink firmware to an ESP32 (4 MB flash). Keeps NVS, so Bluetooth pairings survive.
# Usage: tools\flash_pure_sink.ps1 [-Port COM7]
param([string]$Port = 'COM7')
$repo = Split-Path -Parent $PSScriptRoot
$b = Join-Path $repo 'firmware\native_bt\pure_sink\build'
$env:IDF_PATH = Join-Path $repo 'firmware\esp-idf'
. (Join-Path $env:IDF_PATH 'export.ps1') | Out-Null
python -m esptool --chip esp32 -p $Port -b 921600 --before default-reset --after hard-reset write-flash `
    --flash-mode dio --flash-freq 80m --flash-size 4MB `
    0x1000 "$b\bootloader\bootloader.bin" 0x8000 "$b\partition_table\partition-table.bin" 0x10000 "$b\pure_sink.bin"
exit $LASTEXITCODE
