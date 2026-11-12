# StackChan Factory Firmware

Partition table read from the device's factory flash image.

## Partition Table

|Name|Type|Offset|Size|
|-|-|-|-|
|(bootloader)|-|0x000000|-|
|(partition table)|-|0x008000|-|
|nvs|data/nvs|0x009000|16 KB|
|otadata|data/ota|0x00D000|8 KB|
|phy\_init|data/phy|0x00F000|4 KB|
|ota\_0|app|0x020000|5056 KB|
|ota\_1|app|0x510000|5056 KB|
|assets|data/0x82|0xA00000|4096 KB|
|coredump|data/coredump|0xE00000|64 KB|

## Backup

* File: `stackchan-factory-20261025.bin` (16 MB, full flash dump)
* Location: `backups/` on the build PC (git-ignored, not in the remote repository) and `~/backup/` on the Raspberry Pi
* SHA256: `e0bd4f7af5074634b84c25dd54d390874a51777547a997f589bfe555978f7337`

