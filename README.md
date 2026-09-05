# ESP32-S3 Groq Speech-to-Text ⚡

Real-time Speech-to-Text on the ESP32 S3 in ~0.3 seconds — completely **FREE**, powered by [Groq](https://groq.com)'s LPU-based cloud AI.

Unlike typical cloud STT services, Groq runs open-source models (from OpenAI, Meta, etc.) on custom Language Processing Units (LPUs) instead of GPUs, giving near-instant responses. This makes it ideal for **voice command projects** that need to feel truly responsive.

▶️ **Watch the full tutorial:** https://youtu.be/r5DwtERVe0Q

---

## ✨ Features

- Speech-to-Text conversion in ~0.3 seconds
- Works for both short commands and longer spoken queries
- Runs on ESP32 S3 with an I2S microphone
- Free tier available (no cost to get started)

---

## 🛠️ Hardware Required

| Component | Notes |
|---|---|
| ESP32 S3 Dev Board | [Buy here](https://techiesms.com/product/esp32-s3-board-16mb-flash-n16r8/) |
| INMP441 I2S Microphone Module | [Buy here](https://techiesms.com/product/inmp441-microphone-module-i2s/) |

## 🔌 Wiring

| INMP441 Pin | ESP32 S3 Pin |
|---|---|
| VDD | 3.3V |
| GND | GND |
| SD | *(update per your wiring diagram)* |
| SCK | *(update per your wiring diagram)* |
| WS | *(update per your wiring diagram)* |
| L/R | GND (left channel) |

> See the video for the full connection diagram.

---

## 📦 Software Requirements

- [Arduino IDE](https://www.arduino.cc/en/software)
- ESP32 boards package **v3.3.11** (install via Boards Manager)
- A free [Groq Console](https://console.groq.com) account + API key

---

## 🚀 Setup Instructions

1. **Clone this repo**
   ```bash
   git clone https://github.com/yourusername/esp32-groq-speech-to-text.git
   ```

2. **Get your Groq API key**
   - Sign up at [console.groq.com](https://console.groq.com)
   - Generate an API key from the dashboard

3. **Configure `secrets.h`**
   Open `secrets.h` and fill in:
   ```cpp
   #define GROQ_API_KEY "your_api_key_here"
   #define WIFI_SSID "your_wifi_ssid"
   #define WIFI_PASSWORD "your_wifi_password"
   ```

4. **Install ESP32 board package v3.3.11**
   Make sure this exact version is installed in Arduino IDE's Boards Manager — other versions may fail to compile.

5. **Wire the INMP441 mic to your ESP32 S3** as shown in the diagram above / in the video.

6. **Select your board and COM port**, then hit Upload.

7. **Open the Serial Monitor** and start talking — your speech will be converted to text in real time.

---

## 💰 Pricing / Free Tier

Groq's Speech-to-Text API is free to use but comes with rate limits. Check current limits on the [Groq Console](https://console.groq.com) / [Pricing page](https://groq.com/pricing).

---

## 🛒 Where to Buy the Parts

Get the ESP32 S3 and INMP441 mic at the lowest prices on our store:
- [ESP32 S3 Dev Board](https://techiesms.com/product/esp32-s3-board-16mb-flash-n16r8/)
- [INMP441 Microphone Module](https://techiesms.com/product/inmp441-microphone-module-i2s/)

---

## 📺 More Projects

Check out [Techiesms on YouTube](https://youtube.com) for more ESP32, IoT, and embedded AI projects.

## 📄 License

MIT — free to use, modify, and share.
