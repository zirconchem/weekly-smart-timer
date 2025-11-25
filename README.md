# Weekly Smart Timer (ESP32 + DS3231)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/Platform-ESP32-green.svg)
![Framework](https://img.shields.io/badge/Framework-Arduino-blue.svg)
![Status](https://img.shields.io/badge/Status-Active-brightgreen.svg)
![Version](https://img.shields.io/badge/Release-v1.0.0-orange.svg)

A robust **Weekly Smart Timer** built on **ESP32 WROOM + DS3231 RTC**, supporting **8 programmable daily intervals**, with **NTP fallback**, persistent configuration via **LittleFS**, and a clean mobile-friendly **Web UI** for management.

This project is designed for industrial and home automation tasks requiring accurate repeatable scheduling, even without internet.

---

## ✨ Features

- **ESP32 WROOM** for WiFi + high reliability
- **DS3231 RTC** for high-accuracy timekeeping
- **Automatic NTP time sync** when WiFi is available  
  (used if RTC has failed or is invalid)
- **8 programmable daily intervals**
- **Web-based configuration panel**  
  - Edit schedule  
  - Manual override  
  - WiFi setup  
  - Set time from browser  
  - Shows current mode, relay state, temperature
- **Persistent storage using LittleFS**
- **Hotspot/AP mode** if WiFi not configured
- **Fail-safe time handling** (RTC watchdog → NTP sync)
- **Relay control with selectable active-low logic**

---

## 📱 Web Dashboard Screenshot
*(More screenshots can be added later)*

### Main Dashboard
![Dashboard](docs/Dashboard.jpg)

---

## 🧰 Hardware Requirements

| Component        | Description                         | Notes                                |
|------------------|-------------------------------------|---------------------------------------|
| ESP32 WROOM      | Main microcontroller                | GPIO27 = Relay, GPIO2 = Status LED    |
| DS3231 RTC       | Real-time clock (I²C)               | Address `0x68`                        |
| Relay Module     | 5V/3.3V relay driver                | Active-LOW supported                  |
| Power Supply     | Stable 5V for ESP32 + peripherals   |                                       |

---

## 🔌 Wiring Diagram
Full schematic (SVG):

![Schematic](docs/Esp32Project.SVG)

---

## 📂 Folder Structure

weekly-smart-timer/
├── src/
│   └── WeeklySmartTimer.ino
├── README.md
└── docs/
    ├── Dashboard.jpg
	├── Schedule.jpg
	├── Overrride.jpg
    └── Esp32Project.SVG

---

## 🔧 Installation

### **1. Install ESP32 Board Support**
Arduino IDE →  
**File → Preferences → Additional Boards Manager URLs**
https://dl.espressif.com/dl/package_esp32_index.json


Then open:  
**Tools → Board Manager → ESP32 → Install**

---

### **2. Required Libraries**
Install via Arduino Library Manager:

- `RTClib` (Adafruit)
- `ArduinoJson`
- `LittleFS_esp32`
- `WebServer` (built in ESP32 core)
- `time.h` (built-in)

---

### **3. Upload Firmware**
1. Board: **ESP32 Dev Module**  
2. Partition Scheme: **Default 4MB with LittleFS**  
3. Upload `.ino` file  
4. (Optional) Upload LittleFS data using the plugin

---

## 🌐 Web Interface

### Modes
- **STA Mode** (WiFi configured)
- **AP Mode** (fallback)  
  - SSID: `Smart_Timer`  
  - Password: `timer1234`  
  - IP: `192.168.4.1`

### UI Sections
- Dashboard (time, relay status, RTC state)
- Schedule Editor (8 intervals × 7 days)
- Manual Override
- WiFi Setup
- Set Time (browser → RTC)

---

## 🕒 Timekeeping Logic

1. **Primary:** DS3231 RTC  
2. **If RTC invalid:**  
   → Enable STA Mode → Sync with NTP  
3. **On success:**  
   → Write correct time back to RTC  
4. **Schedules operate in local time (PKT / UTC+5)**  
5. Internal system time uses UTC via `settimeofday()`

This ensures accurate operation even after:
- Power failures  
- Internet outages  
- RTC battery depletion  

---

## 🧪 Verified Configuration

- ESP32 WROOM Dev Module  
- DS3231 RTC with CR2032  
- LittleFS filesystem  
- Firmware size ~466 KB  
- Works fully offline  
