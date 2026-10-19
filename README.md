# Bloomshoot

Bloomshoot is a six-lamp wooden light box for photographing jars and other small objects. This repository currently contains the first ESP32-S3 lighting controller firmware.

## Controls

The 128x64 OLED shows the lamp being edited, its color, brightness, and on/off state.

| Touch pad | Action |
|---|---|
| UP / DOWN | Select lamp 1–6 |
| LEFT / RIGHT | Lower / raise the selected lamp brightness by 5% |
| A | Cycle the selected lamp through the color presets |
| B | Toggle the selected lamp on or off |

Each lamp stores its own color, brightness, and on/off state in ESP32 NVS. Settings save about 1.2 seconds after the last adjustment. A full flash erase clears these settings.

## Current firmware target

- ESP32-S3 full-size board described in the Happy Jarz hardware reference: 16 MB flash, 8 MB OPI PSRAM.
- Six APA106 addressable RGB LEDs in one serial chain, data on GPIO7.
- SSD1306 128x64 I2C OLED at address 0x3C: SDA GPIO8, SCL GPIO6.
- Capacitive touch pads: UP GPIO4, DOWN GPIO5, LEFT GPIO9, RIGHT GPIO10, A GPIO1, B GPIO2.
- Arduino library dependency: U8g2.

The pin assignments are carried forward from the tested Happy Jarz ESP32-S3 touch/OLED/APA106 platform. Bloomshoot’s six-LED chain and OLED are a new combination and still need bench validation. The firmware uses the Happy Jarz RGB byte order and RMT timing; verify the colors on one physical LED before connecting the full chain.

## Power and first-light test

1. With USB and battery disconnected, check polarity and continuity.
2. Use a regulated 5 V supply sized for the six LEDs and ESP32 load. Do not power LEDs from an ESP32 GPIO or the 3.3 V rail. Keep grounds common.
3. Use the Happy Jarz level-shifting path for LED data (SN74AHCT125), with its 100 nF bypass capacitor, a 220–470 Ω series resistor near the first LED DIN, and a 220–470 µF capacitor across the LED supply near the chain.
4. Meter-check the supply at 5.0 V before connecting the ESP32 or LEDs. Test one LED first at low brightness; check RGB order and DIN direction, then add the remaining LEDs one at a time.
5. Six APA106 lamps can draw substantially more current than the four-lamp Happy Jarz chain. Confirm the supply/boost module can handle the measured load and keep brightness low during bring-up.

## Build and upload

1. Install the Espressif ESP32 Arduino board package and the **U8g2** library in Arduino IDE.
2. Open `firmware/Bloomshoot/Bloomshoot.ino`.
3. Select **ESP32S3 Dev Module**. Set flash size to **16 MB** and PSRAM to **OPI PSRAM** for the documented N16R8 board. Choose a partition layout that fits the selected board package.
4. Connect the board through its **UART** USB-C port for upload. Select its serial port. For UART diagnostics, leave USB CDC on boot disabled so `Serial` uses UART0.
5. For the requested blank start, use the board menu’s **Erase All Flash Before Sketch Upload** option once, then upload. This erases the complete ESP32 flash, including stored settings and any previous firmware; it is not needed for normal firmware updates.
6. Start with the LED chain disconnected or one LED attached, then power up and inspect the OLED and serial monitor at 115200 baud.

If the board package does not expose the full-erase menu, the equivalent esptool action is `python -m esptool --chip esp32s3 --port COMx erase-flash`, replacing `COMx` with the board’s actual port. Older esptool releases may spell the subcommand `erase_flash`.

## Source

The Happy Jarz touch input map, OLED configuration, APA106 RMT signal timing, and wiring cautions are adapted from [HappyJarzESP32S3MiniPublic](https://github.com/Etsimulocto/HappyJarzESP32S3MiniPublic) and the project’s October 2026 hardware reference. Bloomshoot is a separate sketch with a six-lamp camera-light interface.
