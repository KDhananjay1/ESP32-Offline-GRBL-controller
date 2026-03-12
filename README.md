# ESP32 GRBL CNC Controller

DIY offline CNC controller using ESP32 with LCD interface, SD card G-code execution and WiFi web control.

## Features

* ESP32 based GRBL controller
* 16x2 I2C LCD menu interface
* 4 button navigation
* SD card G-code execution
* WiFi web interface
* File upload from browser
* Jog control (X/Y/Z)
* Pause / Resume / Stop
* Job progress %
* Buzzer feedback
* EEPROM settings

## Hardware

* ESP32 DevKit
* 16x2 I2C LCD (0x27)
* 4 Push Buttons
* Micro SD Card Module
* Buzzer
* GRBL CNC Controller

## Wiring

### LCD

| LCD | ESP32  |
| --- | ------ |
| SDA | GPIO21 |
| SCL | GPIO22 |

### Buttons

| Button | GPIO |
| ------ | ---- |
| UP     | 32   |
| DOWN   | 33   |
| SELECT | 25   |
| BACK   | 26   |

### SD Card

| SD   | ESP32 |
| ---- | ----- |
| CS   | 5     |
| MOSI | 23    |
| MISO | 19    |
| SCK  | 18    |

### GRBL Serial

| ESP32 | GRBL |
| ----- | ---- |
| TX17  | RX   |
| RX16  | TX   |

## WiFi Web Interface

```
SSID: ESP32-CNC
Password: 12345678
```

Open browser:

```
192.168.4.1
```

Features:

* machine status
* jog control
* file upload
* run G-code

## Menu Structure

Main Menu

```
Control
Status
SD Card
WiFi
Settings
```

## Supported Gcode Files

```
.nc
.gcode
.gco
.txt
```

## License

MIT License
