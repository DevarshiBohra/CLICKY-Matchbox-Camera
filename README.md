# CLICKY 📷
 
> A standalone, wireless point-and-shoot camera — built from scratch on an ESP32-CAM module.
 
CLICKY is a DIY digital camera that captures full-resolution JPEG photos to a microSD card, shows a live preview on a small TFT screen, and lets you browse, download, or delete your photos from any phone or laptop browser over WiFi — no app, no internet, no cables required. Firmware can be updated over the air too.
 
---
 
## Table of Contents
 
- [What it does](#what-it-does)
- [Hardware you need](#hardware-you-need)
- [Wiring guide](#wiring-guide)
- [Software setup](#software-setup)
- [Partition scheme — important](#partition-scheme--important)
- [Libraries to install](#libraries-to-install)
- [Flashing the firmware](#flashing-the-firmware)
- [How to use CLICKY](#how-to-use-clicky)
  - [Live preview](#live-preview)
  - [Taking a photo](#taking-a-photo)
  - [Grayscale mode](#grayscale-mode)
  - [WiFi gallery mode](#wifi-gallery-mode)
  - [OTA firmware update](#ota-firmware-update)
- [Button behaviour at a glance](#button-behaviour-at-a-glance)
- [The progress bar explained](#the-progress-bar-explained)
- [File naming on the SD card](#file-naming-on-the-sd-card)
- [EEPROM — remembering your settings](#eeprom--remembering-your-settings)
- [Web gallery features](#web-gallery-features)
- [OTA update workflow](#ota-update-workflow)
- [Troubleshooting](#troubleshooting)
- [Technical details](#technical-details)
- [Project structure](#project-structure)
- [Credits](#credits)
---
 
## What it does
 
| Feature | Detail |
|---|---|
| Live preview | Real-time viewfinder on a 160×80 TFT display |
| Photo capture | Full UXGA (1600×1200) JPEG saved to SD card |
| Grayscale mode | Hardware-level B&W toggle, persists across reboots |
| WiFi gallery | Browse, download, and delete photos from any browser |
| Captive portal | Opening any URL on the CLICKY network takes you to the gallery |
| OTA updates | Push new firmware wirelessly — no USB cable needed after first flash |
| No internet needed | Everything runs as a self-contained WiFi access point |
 
---
 
## Hardware you need
 
| Part | Notes |
|---|---|
| ESP32-CAM (AI Thinker) | The main board — includes the OV2640 camera |
| FTDI USB-to-Serial adapter (3.3 V) | Only needed for the very first flash |
| ST7735 TFT display (160×128) | Small colour screen for the viewfinder |
| MicroSD card | FAT32 formatted, 32 GB or smaller recommended |
| Tactile push button | Momentary normally-open button |
| 10 kΩ resistor | Pull-up for the button (if not using internal pull-up) |
| Jumper wires | Male-to-female or male-to-male depending on your setup |
| Breadboard or custom PCB | For assembling everything |
| 5 V power supply (≥ 500 mA) | USB power bank works perfectly |
 
> **Note:** The AI Thinker ESP32-CAM does not have a built-in USB port. You need the FTDI adapter only once, for the initial flash. After that, all updates happen wirelessly over OTA.
 
---
 
## Wiring guide
 
### ESP32-CAM → TFT Display (ST7735)
 
The TFT uses SPI. The ESP32-CAM shares SPI pins with the SD card slot, so the CS (chip select) pin is what separates them.
 
| TFT Pin | ESP32-CAM GPIO | Notes |
|---|---|---|
| VCC | 3.3 V | |
| GND | GND | |
| SCL (CLK) | GPIO 14 | SPI clock |
| SDA (MOSI) | GPIO 13 | SPI data |
| RES (RESET) | GPIO 15 | Display reset |
| DC (A0) | GPIO 2 | Data/command select |
| CS | GPIO 12 | **TFT_CS — defined in code** |
| BLK (backlight) | 3.3 V or GPIO | Tie to 3.3 V for always-on |
 
> The pin numbers above must match your `User_Setup.h` file inside the TFT_eSPI library. See the Software Setup section for details.
 
### Button
 
| Button pin | ESP32-CAM GPIO | Notes |
|---|---|---|
| One leg | GPIO 3 (RX0) | Defined as `BUTTON_PIN` in code |
| Other leg | GND | Internal pull-up is enabled in code |
 
> GPIO 3 is the RX0 serial pin. It works fine as a button input when you are not actively using the Serial monitor. Once you have flashed the board and are done debugging, this is perfectly safe to use.
 
### SD Card
 
The SD card is built into the AI Thinker ESP32-CAM module — you do not need to wire anything for it. Just format your microSD card as FAT32 and insert it.
 
---
 
## Software setup
 
### Step 1 — Install Arduino IDE
 
Download and install Arduino IDE 2.x from [arduino.cc](https://www.arduino.cc/en/software).
 
### Step 2 — Add ESP32 board support
 
1. Open Arduino IDE → **File → Preferences**
2. In the "Additional boards manager URLs" field, paste:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click OK
4. Go to **Tools → Board → Boards Manager**
5. Search for `esp32` and install the package by **Espressif Systems** (version 2.x recommended)
### Step 3 — Select the correct board
 
Go to **Tools → Board → ESP32 Arduino** and select:
 
```
AI Thinker ESP32-CAM
```
 
---
 
## Partition scheme — important
 
This is the most important setting and it is easy to miss.
 
The default partition scheme does not have space for OTA updates. You must change it:
 
**Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA)**
 
This creates two equal app slots so the board can hold a running firmware and a new one at the same time, swapping over safely when an update completes.
 
> If you skip this step, the board will still work but OTA updates will fail silently or corrupt the firmware.
 
Other board settings to check:
 
| Setting | Value |
|---|---|
| Board | AI Thinker ESP32-CAM |
| Upload Speed | 115200 |
| Flash Frequency | 80 MHz |
| Flash Mode | QIO |
| Partition Scheme | Minimal SPIFFS (1.9MB APP with OTA) |
| Core Debug Level | None |
| Port | Your FTDI COM port |
 
---
 
## Libraries to install
 
Go to **Tools → Manage Libraries** and install each of these:
 
| Library | Author | What it does |
|---|---|---|
| TFT_eSPI | Bodmer | Drives the ST7735 TFT display |
| esp32-camera | Espressif | Camera driver (usually included with ESP32 board package) |
 
The following are part of the ESP32 Arduino core and do not need separate installation:
 
- `WiFi.h`
- `WebServer.h`
- `DNSServer.h`
- `Update.h`
- `EEPROM.h`
- `SD_MMC.h`
- `FS.h`
### Configuring TFT_eSPI
 
After installing TFT_eSPI you must edit its configuration file, otherwise the display will not work.
 
1. Find the library folder. On Windows it is usually:
   ```
   C:\Users\YourName\Documents\Arduino\libraries\TFT_eSPI\
   ```
2. Open the file `User_Setup.h` in any text editor
3. Comment out all driver definitions and uncomment only:
   ```cpp
   #define ST7735_DRIVER
   ```
4. Set the pin definitions to match the wiring table above:
   ```cpp
   #define TFT_CS   12
   #define TFT_DC    2
   #define TFT_RST  15
   #define TFT_MOSI 13
   #define TFT_SCLK 14
   ```
5. Set the display dimensions:
   ```cpp
   #define TFT_WIDTH  128
   #define TFT_HEIGHT 160
   ```
6. Save the file
---
 
## Flashing the firmware
 
You only need the FTDI adapter for this first flash. After this, everything is wireless.
 
### Wiring the FTDI adapter
 
| FTDI pin | ESP32-CAM pin |
|---|---|
| GND | GND |
| VCC (3.3 V) | 3.3 V |
| TX | GPIO 3 (RX0) |
| RX | GPIO 1 (TX0) |
 
To put the ESP32-CAM into flash mode, connect **GPIO 0 to GND** before powering on. You can use a jumper wire. Once flashed, remove that wire and reset the board.
 
### Steps
 
1. Wire up the FTDI as above and connect GPIO 0 to GND
2. Plug the FTDI into your computer
3. Open the project `.ino` file in Arduino IDE
4. Select the correct COM port under **Tools → Port**
5. Click **Upload** (the arrow button)
6. Once you see "Connecting...", press the RST button on the ESP32-CAM if the upload does not start automatically
7. Wait for "Done uploading"
8. Remove the GPIO 0 to GND jumper wire
9. Press RST to restart the board normally
The TFT should light up and show a live camera preview. You are done with the cable — from here everything is wireless.
 
---
 
## How to use CLICKY
 
### Live preview
 
As soon as the board starts, the TFT display shows a live viewfinder from the camera. The preview runs at QQVGA resolution (160×120) to keep it smooth. No button press is needed — it just runs.
 
### Taking a photo
 
Press the button briefly (less than 3 seconds) and release it. The display will show "Capturing..." and the board saves a full-resolution JPEG to the SD card, then restarts and returns to the live preview. Photos are saved as `pic1.jpg`, `pic2.jpg`, and so on — the board finds the next available number automatically.
 
### Grayscale mode
 
CLICKY supports a hardware grayscale mode using the OV2640 camera sensor's built-in effect engine. This means the grayscale conversion happens inside the camera chip itself before the image is even processed — it costs nothing and works at full resolution.
 
**To toggle grayscale on or off:**
 
Press and hold the button until the progress bar on screen turns green (between 3 and 5 seconds), then release. The board will restart in the new mode.
 
- If you were in colour mode, it switches to grayscale
- If you were in grayscale, it switches back to colour
The selected mode is saved to EEPROM (a tiny persistent memory chip on the board) so it survives power cuts and restarts. Every time CLICKY boots, it reads the saved mode and applies it automatically.
 
When grayscale is active, the live preview on the TFT and all captured photos will be in black and white.
 
### WiFi gallery mode
 
Press and hold the button for 5 seconds or more (until the progress bar turns blue), then release. The board starts a WiFi access point.
 
**To access your photos:**
 
1. On your phone or laptop, open WiFi settings
2. Connect to the network named **CLICKY** (no password)
3. Open any web browser
4. Type `192.168.4.1` in the address bar, or just try opening any website — the captive portal will redirect you automatically
5. The gallery page will load showing all photos on the SD card
### WiFi gallery features
 
Once in the gallery:
 
- **View** — all photos load as thumbnails on the page
- **Download selected** — tick the photos you want and tap "Download selected"
- **Delete selected** — tick photos and tap "Delete selected" to remove them from the SD card
- **Delete all** — removes every photo from the SD card in one go (asks for confirmation)
- **Stop WiFi** — exits WiFi mode and restarts the board, returning to camera mode
### OTA firmware update
 
When in WiFi mode, you (and only you, with the password) can push new firmware wirelessly.
 
1. Connect to the CLICKY WiFi network
2. Open your browser and go to `http://192.168.4.1/update`
3. Your browser will ask for a username and password:
   - Username: `admin`
   - Password: `changeme123` *(change this in the source code before flashing)*
4. Drag your new `.bin` firmware file onto the page, or click to browse for it
5. Click **Flash firmware**
6. A progress bar shows the upload. When done, the board restarts automatically with the new firmware
> **Where to get the `.bin` file:** In Arduino IDE, go to **Sketch → Export Compiled Binary**. It saves a file ending in `.ino.bin` in the same folder as your sketch. That is the file to upload — not the `.merged.bin` or `.bootloader.bin`.
 
> **Critical rule:** Every firmware version you flash must include the OTA code. If you flash a version without it, you will lose the ability to do wireless updates and will need the FTDI cable again.
 
---
 
## Button behaviour at a glance
 
| Press duration | What happens |
|---|---|
| 0 – 3 seconds | Short press — takes a photo |
| 3 – 5 seconds | Toggle press — switches grayscale on or off, then restarts |
| 5+ seconds | Long press — starts WiFi gallery mode |
 
---
 
## The progress bar explained
 
When you press and hold the button, a progress bar appears on the TFT screen. The colour tells you which action will trigger when you release:
 
| Bar colour | Time range | Release action |
|---|---|---|
| White | 0 – 3 seconds | Capture photo |
| Green | 3 – 5 seconds | Toggle grayscale |
| Blue | 5+ seconds | Start WiFi mode |
 
The text label above the bar also updates:
 
- **"Hold for toggle..."** — in the green zone, release here to toggle grayscale
- **"Hold for WiFi..."** — in the blue zone, release here for WiFi mode
If you release the button while the bar is still white (under 3 seconds), the photo is taken immediately.
 
---
 
## File naming on the SD card
 
Photos are saved in the root directory of the SD card as:
 
```
pic1.jpg
pic2.jpg
pic3.jpg
...
```
 
The board scans for the lowest available number each time it captures, so there are no overwrites and no gaps caused by deleted files being reused immediately.
 
---
 
## EEPROM — remembering your settings
 
The board uses 1 byte of EEPROM to store the active camera effect:
 
- `0` = normal colour mode
- `2` = grayscale mode (value `2` matches the OV2640 sensor's effect index)
Every time you toggle grayscale, the new value is written to EEPROM address 0. Every time the board boots, it reads this address and applies the effect before starting the preview. This means your preferred mode is always preserved even if the board loses power.
 
---
 
## Web gallery features
 
The gallery page is a self-contained single-page app served directly from the ESP32's flash memory. It requires no internet connection and works on any modern browser including mobile Safari and Chrome.
 
Features:
 
- Responsive grid layout — works on phones, tablets, and desktops
- Lazy loading thumbnails — images load as you scroll
- Multi-select with checkboxes — tap a photo to select it
- Batch download — selected photos download one by one
- Batch delete — removes selected photos from SD with confirmation
- Delete all — one-tap wipe with confirmation dialog
- Toast notifications — brief on-screen feedback for actions
- Dark mode aware — follows your system preference automatically
---
 
## OTA update workflow
 
Here is the complete workflow for pushing a code change without a cable:
 
1. Make your changes in Arduino IDE
2. Go to **Sketch → Export Compiled Binary** — wait for compilation
3. Find the `.ino.bin` file in your sketch folder (not `.merged.bin`)
4. Power on CLICKY
5. Hold the button for 5+ seconds → connect to CLICKY WiFi
6. Go to `http://192.168.4.1/update` in your browser
7. Log in with your credentials
8. Drag the `.bin` file onto the page → click Flash
9. Wait for the board to restart
10. Confirm the new firmware is running (check serial output or test new features)
---
 
## Troubleshooting
 
**Display shows nothing / stays black**
 
Check your `User_Setup.h` pin definitions in the TFT_eSPI library. The most common cause is the CS, DC, or RST pins being wrong. Also confirm your wiring matches exactly.
 
**"Camera FAILED!" on screen at boot**
 
The camera failed to initialise. This can happen if the board loses power during a previous frame capture. Press the RST button to restart. If it persists, check that the camera ribbon cable on the AI Thinker module is fully seated.
 
**SD card not detected during capture**
 
Make sure the SD card is FAT32 formatted and is 32 GB or smaller. ExFAT and NTFS are not supported. Try reformatting the card using the official SD Card Formatter tool from sdcard.org.
 
**WiFi network "CLICKY" does not appear**
 
Confirm the board has fully booted (display shows WiFi mode screen). The AP can take a few seconds to appear after the board starts. If it still does not appear, open the serial monitor at 115200 baud and look for the "AP OK" message.
 
**OTA page asks for password but won't accept it**
 
Make sure you are typing the credentials exactly as defined in the source code (`OTA_USERNAME` and `OTA_PASSWORD`). Some browsers cache a failed login — try an incognito/private window.
 
**OTA upload fails halfway**
 
The `.bin` file may be too large for the partition scheme you selected. Confirm you are using "Minimal SPIFFS (1.9MB APP with OTA)" and that the sketch size is under 1.9 MB (check the output panel in Arduino IDE after compiling).
 
**Photos are all black or very dark**
 
This can happen if auto-exposure has not had time to adjust when the capture is triggered. The code already discards two warm-up frames before saving. If it persists, try capturing in better lighting.
 
**Button does nothing**
 
GPIO 3 is also the serial RX pin. If you have the serial monitor open, it can interfere. Close the serial monitor and try again. Also check the button wiring — one leg to GPIO 3, the other to GND.
 
---
 
## Technical details
 
| Property | Value |
|---|---|
| Microcontroller | ESP32-S (dual-core 240 MHz) |
| Camera sensor | OV2640 |
| Preview resolution | QQVGA 160×120 RGB565 |
| Capture resolution | UXGA 1600×1200 JPEG (with PSRAM), SVGA fallback |
| Display | ST7735 160×128 TFT via SPI |
| Storage | MicroSD via SD_MMC (1-bit mode) |
| WiFi | 802.11 b/g/n, AP mode only |
| AP IP address | 192.168.4.1 |
| OTA method | HTTP multipart upload via ESP32 Update library |
| EEPROM usage | 1 byte at address 0 (camera effect index) |
| Flash size | 4 MB |
| PSRAM | 4 MB (used for large frame buffers) |
| Partition scheme | Minimal SPIFFS — two 1.875 MB OTA app slots |
 
---
 
## Project structure
 
```
clicky/
├── esp32_cam_final_OTA.ino   ← Main sketch
├── README.md                 ← This file
└── build/
    ├── esp32_cam_final_OTA.ino.bin           ← Flash this for OTA
    ├── esp32_cam_final_OTA.ino.merged.bin    ← Full image for USB flash from scratch
    └── esp32_cam_final_OTA.ino.bootloader.bin
```
 
---
 
Built with:
- [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) by Espressif Systems
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) by Bodmer
- [esp32-camera](https://github.com/espressif/esp32-camera) by Espressif Systems
---
 
*A photo is forever.*
