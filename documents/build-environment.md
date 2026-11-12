# Build Environment

Hosts used to build and flash the StackChan firmware, and the programs installed on each.

## Network

| Host | Address | Notes |
|---|---|---|
| Build PC (Arch Linux laptop) | DHCP (Wi-Fi) | Builds the firmware |
| Raspberry Pi 3 | `rpi3.local` (mDNS; MAC `b8:27:eb:59:c2:ca`) | Flash host and serial host |

- IPs are DHCP-assigned and may change; always use `rpi3.local`
- Fallback if mDNS fails: `ip neigh | grep b8:27:eb`

### SSH

- Key: `~/.ssh/id_ed25519` (ed25519, **no passphrase**), installed on the Pi with `ssh-copy-id`
- Agent: systemd user socket `ssh-agent.socket` (enabled), `SSH_AUTH_SOCK` set in `~/.bashrc` and `~/.config/environment.d/ssh-agent.conf`
- `~/.ssh/config`:

```
Host rpi3 rpi3.local
    HostName rpi3.local
    HostKeyAlias rpi3         # host key stays valid when the IP changes
    AddressFamily inet        # mDNS also returns an IPv6 link-local address without scope
```

- Login: `ssh rpi3` (user `hwanhee`)

### mDNS on the Build PC

Arch does not resolve `*.local` by default (Windows 10/11 and macOS do). Installed:

```sh
sudo pacman -S --needed avahi nss-mdns
sudo systemctl enable --now avahi-daemon
sudo sed -i 's/^hosts: mymachines resolve/hosts: mymachines mdns_minimal [NOTFOUND=return] resolve/' /etc/nsswitch.conf
```

Resulting line: `hosts: mymachines mdns_minimal [NOTFOUND=return] resolve [!UNAVAIL=return] files myhostname dns`

## Build PC

- OS: Arch Linux (kernel 6.17), x86_64
- No root needed: ESP-IDF and its tools are installed per user

### System Packages (pacman, already present)

| Program | Version | Use |
|---|---|---|
| git | 2.52.0 | ESP-IDF and submodule checkout |
| python | 3.13.7 | System Python, **not used** for ESP-IDF |
| gcc, make | 15.2.1, 4.4.1 | Host tools built during the IDF build |
| flex, bison | 2.6.4, 3.8.2 | Kconfig tools |
| libusb | 1.0.29 | OpenOCD / USB-JTAG |
| curl | 8.17.0 | Tool downloads |

Optional, not installed (needs sudo): `ccache` (faster rebuilds), `gperf`, `dfu-util` (not used with USB-Serial/JTAG), `rsync` (scripts use tar over ssh instead).

### uv (Python)

| Item | Value |
|---|---|
| uv | 0.13.7, `~/.local/bin/uv` (standalone installer, no sudo) |
| Python | CPython 3.12.16, uv-managed (`~/.local/share/uv/python/`) |

```sh
curl -LsSf https://astral.sh/uv/install.sh | env UV_NO_MODIFY_PATH=1 sh
uv python install 3.12
```

### ESP-IDF

| Item | Value |
|---|---|
| Version | v5.5.4 (same as the device firmware) |
| Location | `~/esp/esp-idf` (shallow clone, ~0.7 GB) |
| Tools | `~/.espressif` (~4.1 GB) |
| Python env | `~/.espressif/python_env/idf5.5_py3.12_env` (created with `uv venv --seed`, uv Python 3.12) |
| Target | `esp32s3` |

Tools in `~/.espressif/tools`:

| Tool | Version |
|---|---|
| xtensa-esp-elf (ESP32-S3 GCC) | esp-14.2.0_20260121 |
| riscv32-esp-elf (LP core) | esp-14.2.0_20260121 |
| esp32ulp-elf | 2.38_20240113 |
| xtensa-esp-elf-gdb | 16.3_20250913 |
| openocd-esp32 | v0.12.0-esp32-20251215 |
| esp-rom-elfs | 20241011 |
| cmake | 3.30.2 |
| ninja | 1.12.1 |
| esptool (Python) | 4.13.0 |

### Install

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.4 --recursive --shallow-submodules --depth 1 \
    https://github.com/espressif/esp-idf.git esp-idf
cd esp-idf
./install.sh esp32s3                             # toolchains (Python env replaced below)
python3 tools/idf_tools.py install cmake ninja   # not on Arch by default

# Python env on uv Python 3.12
UVPY=$(uv python find 3.12)
ENV=~/.espressif/python_env/idf5.5_py3.12_env
uv venv --seed --python "$UVPY" "$ENV"
mkdir -p ~/.local/share/esp-idf-bin
ln -sf "$UVPY" ~/.local/share/esp-idf-bin/python3
ln -sf "$UVPY" ~/.local/share/esp-idf-bin/python
PATH=~/.local/share/esp-idf-bin:$PATH IDF_PYTHON_ENV_PATH=$ENV \
    python3 tools/idf_tools.py install-python-env
```

`~/.bashrc` defines `get_idf`, which puts the uv Python first on `PATH`, sets `IDF_PYTHON_ENV_PATH` and sources `export.sh`.

### Use

```sh
get_idf                        # once per shell
idf.py set-target esp32s3      # once per project
idf.py build
```

- Verified 2026-11-13 by building `examples/get-started/hello_world` for `esp32s3` (uv Python 3.12 env)

## Raspberry Pi 3

| Item | Value |
|---|---|
| Hostname / user | `rpi3` / `hwanhee` (groups include `dialout`; sudo needs a password) |
| OS | Debian GNU/Linux 13 (trixie), aarch64, 905 MB RAM, 15 GB SD |
| mDNS | `avahi-daemon` active |
| System Python | 3.13.5 (not used) |
| uv | 0.13.7, `~/.local/bin/uv` |
| Python | CPython 3.12.16, uv-managed |
| esptool | 4.13.0 (`uv tool install --python 3.12 "esptool~=4.13"`), `~/.local/bin/esptool.py`; same major version as ESP-IDF v5.5 |
| Device port | `/dev/ttyACM0` = `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:45:6B:54:8A:E8-if00` |
| Factory backup | `~/backup/stackchan-factory-20261025.bin` (SHA256 matches [factory-firmware.md](factory-firmware.md)) |
| Serial logger | `~/stackchan-logs/` (`start.sh`, `logger.py`); stop it before flashing |

- `~/.local/bin` is added to `PATH` by `~/.profile` (login shells only); use the full path in `ssh rpi3 '<command>'`
- Verified 2026-11-13: `esptool.py --chip esp32s3 -p /dev/ttyACM0 flash_id` from the Pi (ESP32-S3 v0.2, flash 0x46/0x4018, 16 MB)
