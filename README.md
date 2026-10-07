# ESP32 Spotify Player

A standalone Spotify remote with a touch screen for the **ESP32-2432S028R** ("Cheap Yellow Display", CYD). It shows what is playing on any of your Spotify devices and lets you control it: play/pause, skip, seek, volume, switch device and start playlists. The album art is used as a blurred full-screen background.

The ESP32 does not play audio itself. It controls an existing Spotify Connect device (phone, computer, speaker) through the Spotify Web API.

> Not affiliated with or endorsed by Spotify. Spotify is a trademark of Spotify AB. **Controlling playback requires Spotify Premium.**

## Features

- Now playing screen with title, artist, progress bar and album art as full-screen background; accent colours follow the album art.
- Play/pause, previous/next, volume up/down, and **seek by tapping the progress bar**.
- **Device picker**: transfer playback to any available Spotify Connect device.
- **Playlists**: pinned playlists are shown first, the rest can be loaded in pages of 50 ("Load more").
- **Track details popup** (tap title or artist): album, release date, track number, length, explicit flag, ISRC.
- **Device info popup** (long press anywhere): IP address, Wi-Fi signal, MAC, uptime, heap, Spotify status. Useful for troubleshooting.
- Web interface for setup, with tabs:
  - **Wi-Fi**: network credentials.
  - **Spotify**: client ID, login (PKCE, no client secret needed) and album art resolution.
  - **Playlists**: pin playlists by searching your library, searching all of Spotify, or pasting a playlist URL/ID. Pins can be renamed and reordered.
  - **Debug**: in-browser log with verbose mode and a copy button. Logs are only kept in RAM and shown in the browser.
- Characters outside the built-in font (emoji, CJK) are dropped instead of drawn as boxes. Latin, Cyrillic, Greek and common symbols are supported.

## Hardware

Designed for the **ESP32-2432S028R** (ESP32-WROOM-32, 4 MB flash, no PSRAM, 2.8" 320x240 ILI9341 display, XPT2046 resistive touch, USB-to-serial CH340).

The pin configuration is set with build flags in [platformio.ini](platformio.ini):

| Function | GPIO |
|---|---|
| TFT MOSI / MISO / SCLK / CS / DC | 13 / 12 / 14 / 15 / 2 |
| TFT backlight | 21 |
| Touch CLK / MOSI / MISO / CS / IRQ | 25 / 32 / 39 / 33 / 36 |

Board variants: some CYD boards use an ST7789 display instead (often the ones with two USB ports) and may need a different driver and display inversion (`ST7789_DRIVER`, `TFT_INVERSION_ON`). If colours look inverted on an ILI9341 board, add `-DTFT_INVERSION_ON` to `build_flags`. Touch orientation and calibration are in [src/display.cpp](src/display.cpp).

## Install

### Option A: prebuilt firmware (no tools needed to build)

Download `firmware-merged.bin` from the latest [GitHub release](../../releases) (built automatically by the workflow in `.github/workflows/build.yml`). It is a single image containing bootloader, partition table and app, to be written at offset `0x0`.

#### Flash from the browser (nothing to install)

Use Chrome or Edge on a desktop computer (Web Serial is required) and connect the board over USB.

**esptool-js** (<https://espressif.github.io/esptool-js/>):

1. Press **Connect** and select the serial port of the board (CH340).
2. Set **Flash Address** to `0x0` and choose `firmware-merged.bin` as the file.
3. Press **Program** and wait until it reports 100%. Press the reset button on the board afterwards.

**ESPHome Web** (<https://web.esphome.io>):

1. Press **Connect** and select the serial port.
2. Choose **Install**, select `firmware-merged.bin` and confirm. The image already contains the bootloader, so it is written as a full image.

If the board is not listed, install the CH340 USB driver. If flashing does not start, hold the BOOT button while pressing Connect.

#### Flash with esptool

With [esptool](https://github.com/espressif/esptool) (`pip install esptool`):

```bash
esptool.py --chip esp32 --baud 460800 write_flash 0x0 firmware-merged.bin
```

### Option B: build with PlatformIO

[PlatformIO](https://platformio.org/) is required to build from source.

```bash
pip install platformio
git clone <this repository>
cd "ESP32 Spotify Player"
python -m platformio run -t upload
```

PlatformIO downloads the toolchain and all libraries on the first build. Use `python -m platformio device monitor` for the serial console (115200 baud).

To create a single flashable image yourself:

```bash
python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32 merge_bin -o firmware-merged.bin \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 .pio/build/cyd/bootloader.bin \
  0x8000 .pio/build/cyd/partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/cyd/firmware.bin
```

The firmware uses the `huge_app.csv` partition table (3 MB app, ~0.9 MB LittleFS, no OTA).

## Configuration

### 1. Connect to Wi-Fi

1. On first boot the device starts an access point named **`Spotify-Player-Setup`** and shows the setup instructions and its IP address on screen.
2. Connect your phone or computer to that network and open `http://192.168.4.1`.
3. On the **Wi-Fi** tab enter your network name and password. The device restarts and shows its new IP address on screen.

If the saved network cannot be reached, the device falls back to the access point. If nobody connects to it for 5 minutes it restarts and tries the saved network again.

### 2. Create a Spotify app

1. Go to the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and create an app (the Web API is the only API needed).
2. Add this exact redirect URI: `http://127.0.0.1:8080/callback`
3. Copy the **Client ID**. No client secret is used.

Apps in Spotify's development mode are limited to a small number of allowed users; add your own Spotify account under "User Management" in the dashboard.

### 3. Log in to Spotify

Open the device's web interface on your home network (`http://<device-ip>`), go to the **Spotify** tab and:

1. Enter the Client ID (it is remembered for later re-authorisation) and press **1. Login to Spotify**.
2. Approve the access in the Spotify page. The browser then fails to open `127.0.0.1:8080`; this is expected.
3. Copy the full URL from the address bar, paste it in the field and press **2. Save Spotify Config**.

The PKCE challenge is generated on the ESP32 and only the refresh token is stored (in flash). Requested scopes: `user-read-playback-state`, `user-modify-playback-state`, `playlist-read-private`. After updating from an older version that lacked a scope, simply log in again.

### 4. Optional settings

- **Album art resolution** (Spotify tab): 64, 300 (default) or 640 px. Images are downloaded to a temporary flash file, decoded, and deleted again.
- **Pinned playlists** (Playlists tab): search "My playlists", search Spotify, or paste a playlist URL or ID. Pinned playlists appear first on the display and play without any lookup.

## Using the device

| Action | Result |
|---|---|
| Tap play / prev / next / volume buttons | Control playback on the active device |
| Tap the progress bar | Seek to that position |
| Tap the speaker icon (top right) | Choose the playback device |
| Tap the list icon (top right) | Open playlists (pinned first, then "Load more") |
| Tap title or artist | Track details |
| Long press (0.7 s) anywhere else | Device and network info |
| Tap a popup | Close it |

## Limitations

- Spotify no longer returns its own algorithmic and editorial playlists (Daily Mix, radio, Discover Weekly, ...) to development-mode apps. They do not appear in lists. You can try pinning one by ID, but loading its name or playing it is not guaranteed to work.
- The Web API has no composer or credits data, and popularity and label fields were removed in 2026.
- Memory is tight on the ESP32 without PSRAM. The firmware keeps a single TLS connection to Spotify, retries failed connections, and streams large responses through temporary flash files. If you see `connection refused` in the debug log, it was a TLS memory allocation failure that is retried automatically.
- TLS certificates are **not verified** (`setInsecure()`). Anyone able to intercept your traffic could read the access token. Pin the Spotify root certificate if you need protection against that.
- The setup access point is open and the web interface has no password. Use it on a network you trust.

## Troubleshooting

- Open the **Debug** tab, enable *Verbose logging* and use *Copy* to share the log. Entries show HTTP status codes, timing and free heap.
- After an unexpected restart the log starts with the reset reason (panic, watchdog, brownout). A weak USB supply can cause brownouts.
- Nothing playing or "No active device": start playback once in the Spotify app, then pick the device from the device list.
- Insufficient permission when loading playlists: log in again on the Spotify tab.

## Project layout

```
platformio.ini        Board, libraries, display pin build flags
include/lv_conf.h     LVGL configuration
src/main.cpp          State machine (setup AP -> Wi-Fi -> Spotify ready)
src/spotify.*         Spotify Web API client (runs in its own task)
src/ui.*              LVGL screens, popups, lists, album art
src/web_portal.*      Web interface and JSON endpoints
src/config.*          Settings and pinned playlists (LittleFS)
src/app_log.*         In-RAM log shown in the browser
src/display.*         TFT and touch drivers for LVGL
src/fonts/            Generated LVGL fonts
```

The fonts in `src/fonts` are generated with `lv_font_conv` from Montserrat and DejaVu Sans (see the header of each file for the exact command).

## Credits and licence

Built with [LVGL](https://lvgl.io/), [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder), [ArduinoJson](https://arduinojson.org/), [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) and [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen).

Released under the GNU General Public License v3.0, see [LICENSE](LICENSE).
