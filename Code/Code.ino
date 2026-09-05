// =============================================================================
//  SpeechToText - ESP32-S3 + INMP441 -> Groq Whisper -> text on the Serial Monitor
// =============================================================================
//  Hold the BOOT button, talk, release. The transcript prints on serial.
//  That is the whole program. No LLM, no speaker, no text-to-speech.
//
// -----------------------------------------------------------------------------
//  WIRING   INMP441 -> ESP32-S3
// -----------------------------------------------------------------------------
//    VDD -> 3V3      NOT 5V. 3.3 V part, and its output drives a pin that is
//                    not 5 V tolerant.
//    GND -> GND
//    L/R -> GND      Selects the LEFT channel, the slot this code reads.
//    SD  -> GPIO 4   Mic data out
//    SCK -> GPIO 5   Bit clock   (ESP32 generates it - it is the I2S master)
//    WS  -> GPIO 6   Word select
//
//    The BOOT button is already on the board at GPIO0. Nothing to wire.
//
// -----------------------------------------------------------------------------
//  WHAT TO INSTALL
// -----------------------------------------------------------------------------
//  1. BOARD PACKAGE - "esp32 by Espressif Systems", version 3.3.11
//       Tools -> Board -> Boards Manager, search "esp32".
//       If it is not listed, add under Preferences -> Additional Boards URLs:
//         https://espressif.github.io/arduino-esp32/package_esp32_index.json
//       Source: https://github.com/espressif/arduino-esp32
//
//       Core 2.x CANNOT build this. ESP_I2S and I2S_RX_TRANSFORM_32_TO_16 do
//       not exist there. The symptom is:
//         fatal error: ESP_I2S.h: No such file or directory
//
//  2. EXTERNAL LIBRARIES - none. All three includes ship with the board
//     package. ArduinoJson is not needed: we ask Whisper for
//     response_format=text and get a bare string back.
//     (If you want it later: https://github.com/bblanchon/ArduinoJson )
//
// -----------------------------------------------------------------------------
//  BOARD SETTINGS (Tools menu)
// -----------------------------------------------------------------------------
//    Board           : ESP32S3 Dev Module
//    PSRAM           : DISABLED  - with PSRAM on, the I2S driver puts its
//                      channel object in PSRAM, GDMA rejects it, and
//                      mic.begin() fails with "I2S INIT FAILED".
//    USB CDC On Boot : Disabled for a CP2102/CH340 bridge port,
//                      Enabled for the S3's native USB port.
//                      Wrong choice = blank Serial Monitor.
//
// =============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>
#include "secrets.h"          // SEED_WIFI_SSID, SEED_WIFI_PASS, SEED_GROQ_KEY

#define SAMPLE_RATE   16000   // Whisper's native rate; higher just wastes upload
#define MAX_SECONDS   15      // hard stop, so a stuck button cannot record forever
#define SILENCE_PEAK  300     // below this, treat the clip as silence
#define HP_CUTOFF_HZ  120     // high-pass corner, removes DC and rumble
#define MIC_GAIN      8       // digital gain applied after filtering

#define PIN_BUTTON    0       // BOOT, active LOW
#define PIN_MIC_SD    4
#define PIN_MIC_SCK   5
#define PIN_MIC_WS    6

#define GROQ_HOST     "api.groq.com"
#define STT_MODEL     "whisper-large-v3-turbo"
#define BOUNDARY      "----speechtotext"

static I2SClass        mic;
static WiFiClientSecure net;
static bool            micOk = false;

// Opens the TLS connection to Groq if it is not already up.
//
// This is the reason recording starts instantly. A TLS handshake costs roughly
// a second, and if it happened after the button press you would lose the first
// second of every sentence. So loop() calls this while idle, and by the time
// you press BOOT the socket is already open - all that is left is writing the
// request headers, which takes a few milliseconds.
static bool ensureLink() {
  if (net.connected()) return true;
  net.setInsecure();                       // encrypted, but server not verified
  net.setTimeout(20);
  return net.connect(GROQ_HOST, 443);
}

// ======================================================= 1. AUDIO CLEANUP ===
// The INMP441 puts a large DC offset and a lot of sub-120 Hz rumble on its
// output. Measured on this build, the 20-120 Hz band sat 14-22 dB ABOVE the
// speech bands - so that junk used up all the headroom and the actual voice
// ended up tiny, which is what wrecked recognition accuracy.
//
// Two cascaded one-pole high-pass filters remove it. On a real recording this
// took the crest factor from 4.3 (noise-like) to 8.8 (speech-like) and freed
// about 26x of headroom, which MIC_GAIN then uses.
static float hpA;                       // filter coefficient, computed once
static float hpY1, hpX1, hpY2, hpX2;    // filter state, carried across buffers
                                        // (not y1/x1 - those are math.h Bessel functions)

static void filterReset() {
  float rc = 1.0f / (2.0f * PI * HP_CUTOFF_HZ);
  float dt = 1.0f / SAMPLE_RATE;
  hpA = rc / (rc + dt);
  hpY1 = hpX1 = hpY2 = hpX2 = 0.0f;
}

// Filters and amplifies one buffer of samples in place.
static void filterBlock(int16_t *s, size_t count) {
  for (size_t i = 0; i < count; i++) {
    float x = s[i];
    float a = hpA * (hpY1 + x - hpX1); hpX1 = x; hpY1 = a;   // stage 1
    float b = hpA * (hpY2 + a - hpX2); hpX2 = a; hpY2 = b;   // stage 2
    int32_t v = (int32_t)(b * MIC_GAIN);
    s[i] = v >  32767 ?  32767 : v < -32768 ? -32768 : v;
  }
}

// ============================================================== 2. THE MIC ===
// The INMP441 is a 24-bit mic that sends its data inside 32-bit slots, so we
// open the bus at 32 bits. I2S_RX_TRANSFORM_32_TO_16 then makes the driver
// hand back plain 16-bit samples - exactly what a WAV file wants.
static bool micStart() {
  mic.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD, -1);
  micOk = mic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO)
       && mic.configureRX(SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                          I2S_SLOT_MODE_MONO, I2S_RX_TRANSFORM_32_TO_16);
  Serial.printf("[mic] SD=%d SCK=%d WS=%d -> %s\n", PIN_MIC_SD, PIN_MIC_SCK,
                PIN_MIC_WS, micOk ? "started" : "I2S INIT FAILED");
  return micOk;
}

// ========================================================= 3. READING HTTP ===
// One primitive does all the receiving: read characters until end of line.
// Headers are lines, and chunk sizes are lines, so this covers both.
static String readLine(uint32_t deadline) {
  String s;
  while (millis() < deadline) {
    if (!net.available()) { if (!net.connected()) break; delay(2); continue; }
    char ch = net.read();
    if (ch == '\n') break;
    if (ch != '\r') s += ch;
  }
  return s;
}

// Appends exactly n bytes to out (or fewer, if the server hangs up).
static void readBytes(String &out, long n, uint32_t deadline) {
  while (n > 0 && millis() < deadline) {
    if (!net.available()) { if (!net.connected()) break; delay(2); continue; }
    out += (char)net.read();
    n--;
  }
}

// Reads the whole reply. Returns the HTTP status code and fills *body.
//
// Groq answers with Transfer-Encoding: chunked, meaning the body arrives as
// repeated "<length in hex> CRLF <that many bytes> CRLF", ending at a zero
// length. Read it raw and those hex markers end up spliced into your text.
static int readReply(String *body, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  int  status  = 0;
  bool chunked = false;

  // Status line, then headers, then a blank line.
  for (String line = readLine(deadline); line.length(); line = readLine(deadline)) {
    if (line.startsWith("HTTP/")) status = line.substring(9, 12).toInt();
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0)
      chunked = true;
  }

  if (chunked) {
    for (;;) {
      long n = strtol(readLine(deadline).c_str(), nullptr, 16);
      if (n <= 0) break;                       // a zero-length chunk ends the body
      readBytes(*body, n, deadline);
      readLine(deadline);                      // the CRLF that follows each chunk
    }
  } else {
    readBytes(*body, 1 << 20, deadline);       // no chunking: read until close
  }
  return status;
}

// ========================================================= 4. SENDING HTTP ===
// One piece of a chunked request body: length in hex, CRLF, bytes, CRLF.
static bool sendChunk(const uint8_t *data, size_t len) {
  char header[16];
  int hlen = snprintf(header, sizeof header, "%X\r\n", (unsigned)len);
  return net.write((const uint8_t *)header, hlen) == (size_t)hlen
      && net.write(data, len) == len
      && net.write((const uint8_t *)"\r\n", 2) == 2;
}

// A 44-byte WAV header for 16 kHz mono 16-bit, with both length fields set to
// 0xFFFFFFFF = "unknown". We are still recording when this goes out, so the
// real length does not exist yet. Whisper accepts it, and that is what lets us
// stream instead of buffering.
static void writeWavHeader(uint8_t *h) {
  const uint32_t UNKNOWN = 0xFFFFFFFF, rate = SAMPLE_RATE, byteRate = rate * 2, fmtLen = 16;
  const uint16_t pcm = 1, channels = 1, blockAlign = 2, bits = 16;
  memcpy(h + 0,  "RIFF", 4);     memcpy(h + 4,  &UNKNOWN, 4);
  memcpy(h + 8,  "WAVEfmt ", 8); memcpy(h + 16, &fmtLen, 4);
  memcpy(h + 20, &pcm, 2);       memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &rate, 4);      memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &blockAlign, 2);memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);     memcpy(h + 40, &UNKNOWN, 4);
}

// ======================================================== 5. THE MAIN JOB ====
// Records and uploads at the same time, then returns the transcript.
// Recording runs until BOOT is released, or MAX_SECONDS, whichever is first.
static bool transcribe(String *text) {
  if (!micOk) { Serial.println("[mic] not running"); return false; }

  // --- step 1: send the request headers (the socket is normally already open) ---
  if (!ensureLink()) { Serial.println("[net] connect failed"); return false; }

  net.print(String("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n"
                   "Host: " GROQ_HOST "\r\n"
                   "Authorization: Bearer ") + SEED_GROQ_KEY + "\r\n"
            "User-Agent: SpeechToText/1.0\r\n"     // Groq rejects a missing User-Agent
            "Content-Type: multipart/form-data; boundary=" BOUNDARY "\r\n"
            "Transfer-Encoding: chunked\r\n"       // length is unknown up front
            "Connection: close\r\n\r\n");

  // --- step 2: the form fields, then the WAV header ---
  static const char *FORM =
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n"
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n"
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n"
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  uint8_t wav[44];
  writeWavHeader(wav);
  bool ok = sendChunk((const uint8_t *)FORM, strlen(FORM)) && sendChunk(wav, sizeof wav);

  // --- step 3: record and stream until the button comes up ---
  Serial.println("[rec] listening...");
  filterReset();
  uint8_t  raw[1024];          // bytes straight from the I2S driver
  uint32_t peak = 0, start = millis();
  size_t   total = 0;

  while (ok && millis() - start < MAX_SECONDS * 1000UL) {
    // The 300 ms floor stops a quick tap producing a clip too short to use.
    if (digitalRead(PIN_BUTTON) == HIGH && millis() - start > 300) break;

    size_t n = mic.readBytes((char *)raw, sizeof raw);
    if (!n) { delay(1); continue; }

    // configureRX(..., I2S_SLOT_MODE_MONO, I2S_RX_TRANSFORM_32_TO_16) already
    // hands back ONE mono stream of plain int16 samples, so every sample here
    // is real audio and the buffer goes straight onto the socket.
    //
    // Do NOT "de-interleave" this by keeping every second sample. An earlier
    // version did, on the assumption that the odd slots were empty padding.
    // They are not. That halved the real sample rate to 8 kHz while the WAV
    // header still said 16 kHz, so Whisper heard everything at double speed -
    // which is what made recognition so poor.
    int16_t *in = (int16_t *)raw;
    filterBlock(in, n / 2);              // strip DC/rumble, then amplify
    for (size_t i = 0; i < n / 2; i++) {
      uint32_t mag = in[i] < 0 ? -in[i] : in[i];
      if (mag > peak) peak = mag;
    }
    ok = sendChunk(raw, n);
    total += n;
  }
  uint32_t recordedAt = millis();          // audio is done; the clock for STT starts here
  Serial.printf("[rec] %u bytes, %.1f s, peak %u\n",
                (unsigned)total, (recordedAt - start) / 1000.0f, (unsigned)peak);

  // Whisper never errors on silence - it invents a polite sentence instead
  // ("Thank you." is its favourite), which hides a wiring fault behind what
  // looks like a working transcription. So check for signal ourselves.
  if (peak < SILENCE_PEAK) {
    net.stop();
    Serial.println("[rec] no signal - check VDD on 3V3, L/R to GND, and the SD pin");
    return false;
  }

  // --- step 4: close the request, read the answer ---
  String tail = "\r\n--" BOUNDARY "--\r\n";
  ok = ok && sendChunk((const uint8_t *)tail.c_str(), tail.length())
          && net.write((const uint8_t *)"0\r\n\r\n", 5) == 5;   // 0 = end of body
  if (!ok) { net.stop(); Serial.println("[net] upload aborted"); return false; }

  String body;
  int status = readReply(&body, 20000);
  net.stop();

  // How long Groq took, measured from the moment the last audio sample was
  // captured to the moment the transcript finished arriving.
  Serial.printf("[stt] %u ms\n", (unsigned)(millis() - recordedAt));

  if (status != 200) {
    Serial.printf("[stt] HTTP %d%s: %s\n", status,
                  status == 429 ? " (rate limited, wait for the reset)" : "",
                  body.substring(0, 160).c_str());
    return false;
  }
  body.trim();
  *text = body;
  return body.length() > 0;
}


void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\nSpeechToText - Groq Whisper");
  pinMode(PIN_BUTTON, INPUT_PULLUP);           // BOOT reads LOW when pressed

  WiFi.mode(WIFI_STA);
  WiFi.begin(SEED_WIFI_SSID, SEED_WIFI_PASS);
  Serial.print("[wifi] connecting");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(300); }
  Serial.printf("\n[wifi] %s  rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  micStart();
  Serial.println("\nHold BOOT and talk, release when done.\n");
}

void loop() {
  // While nothing is happening, make sure the connection to Groq is open, so a
  // button press does not have to wait for a TLS handshake. Retry every 2 s if
  // it drops, rather than hammering the server.
  static uint32_t nextTry = 0;
  if (!net.connected() && millis() > nextTry) {
    nextTry = millis() + 2000;
    ensureLink();
  }

  if (digitalRead(PIN_BUTTON) != LOW) { delay(10); return; }   // idle until pressed
  delay(30);                                                   // debounce
  if (digitalRead(PIN_BUTTON) != LOW) return;                  // it was just noise

  String text;
  if (transcribe(&text)) {
    Serial.println("-------------------------------------------");
    Serial.printf("You said: %s\n", text.c_str());
    Serial.println("-------------------------------------------\n");
  }

  while (digitalRead(PIN_BUTTON) == LOW) delay(10);            // wait for release
}
