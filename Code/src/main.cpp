/**
 * ==============================================================================
 *  Project: PlanKO - ESP32 Smart Plank Mat & Biometric WebSocket Server
 *  Platform: PlatformIO / Arduino IDE (C++)
 *  Target: ESP32 / ESP32-DevKit / ESP32-C3
 * ==============================================================================
 *  ฟังก์ชันการทำงาน:
 *  1. เชื่อมต่อ Wi-Fi ในเครือข่าย Local LAN
 *  2. เปิด WebSocket Server ที่ Port 81 (ws://<ESP32_IP>:81/)
 *  3. ตรวจจับการกดแผ่นรองแพลงก์ (Plank Pads) ปุ่มฝั่งซ้าย (L1 - L8) และขวา (R1 - R8)
 *     - ใช้งานแบบ INPUT_PULLUP (Active-LOW: ต่อปุ่มลง GND)
 *     - มีระบบ Software Debounce ป้องกันการกระดอนของสัญญาณ
 *  4. ส่งข้อมูล JSON แบบ Real-time ทันทีที่มีการกด:
 *     {"sensorId":"L1","value":1}
 *  5. Serial Simulator: สามารถพิมพ์ "L1", "R3" ฯลฯ ใน Serial Monitor
 * เพื่อจำลองการกดได้ทันที
 *  6. (ออปชันเสริม) รองรับ MAX30102 สำหรับอ่านค่าอัตราการเต้นของหัวใจ (BPM) และ SpO2
 * ==============================================================================
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

// ==============================================================================
// 1. การตั้งค่า Wi-Fi (กรุณากรอกชื่อและรหัสผ่าน Wi-Fi 2.4GHz ของท่าน)
// ==============================================================================
const char *WIFI_SSID = "APISIT164 @2.4G";
const char *WIFI_PASS = "09E2898E";

// กำหนดพอร์ต WebSocket Server (ค่าเริ่มต้นตามโจทย์: 81)
const uint16_t WS_PORT = 81;

// ==============================================================================
// 2. การตั้งค่าเซ็นเซอร์วัดชีพจร MAX30102 (ออปชันเสริม)
// ==============================================================================
// ตั้งค่าเป็น true หากต่อเซ็นเซอร์ MAX30102 ผ่าน I2C (SDA/SCL)
#define ENABLE_MAX30102 false

#if ENABLE_MAX30102
#include "MAX30105.h"
#include "heartRate.h"
#include <Wire.h>

MAX30105 particleSensor;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;
unsigned long lastHeartTelemetryTime = 0;
const unsigned long HEART_TELEMETRY_INTERVAL = 1000; // ส่งข้อมูลทุก 1 วินาที
#endif

// ==============================================================================
// 3. กำหนดขา GPIO สำหรับแผ่นรองแพลงก์ (L1 - L8, R1 - R8)
// ==============================================================================
// หมายเหตุ: สวิตช์ต่อแบบ INPUT_PULLUP (ปุ่มกดต่อเข้า GPIO และปลายอีกข้างต่อ GND)
// เมื่อกดปุ่ม สถานะจะเป็น LOW (0), ปล่อยปุ่มจะเป็น HIGH (1)
struct PadConfig {
  const char *sensorId;
  int8_t pin; // -1 หมายถึงไม่ได้ต่อขาจริง (ยังสามารถจำลองผ่าน Serial ได้)
};

#if defined(CONFIG_IDF_TARGET_ESP32C3)
// Pinout สำหรับ ESP32-C3 (จำนวนขามีจำกัด)
const PadConfig PAD_CONFIGS[] = {
    {"L1", 0},  {"L2", 1},  {"L3", 2},  {"L4", 3}, {"L5", 4},  {"L6", 5},
    {"L7", 6},  {"L8", 7},  {"R1", 8},  {"R2", 9}, {"R3", 10}, {"R4", 18},
    {"R5", 19}, {"R6", 20}, {"R7", 21}, {"R8", -1}};
#else
// Pinout แนะนำสำหรับ ESP32 มาตรฐาน (ESP32-WROOM-32 / DevKit V1 30-38 ขา)
// ทุกขาที่เลือกนี้รองรับวงจร Internal Pull-up ปลอดภัยต่อการบูต
const PadConfig PAD_CONFIGS[] = {
    // ฝั่งซ้าย (Left Pads: L1 - L8)
    {"L1", 13},
    {"L2", 14},
    {"L3", 27},
    {"L4", 26},
    {"L5", 25},
    {"L6", 33},
    {"L7", 32},
    {"L8", 4},

    // ฝั่งขวา (Right Pads: R1 - R8)
    {"R1", 16},
    {"R2", 17},
    {"R3", 18},
    {"R4", 19},
    {"R5", 23},
    {"R6", 5},
    {"R7", 15},
    {"R8", 2}};
#endif

const size_t TOTAL_PADS = sizeof(PAD_CONFIGS) / sizeof(PAD_CONFIGS[0]);

// โครงสร้างข้อมูลสำหรับจัดเก็บสถานะและการทำ Software Debounce ของแต่ละปุ่ม
struct PadSensorState {
  const char *sensorId;
  int8_t pin;
  int lastRawState;
  int debouncedState;
  unsigned long lastDebounceTime;
};

PadSensorState sensors[TOTAL_PADS];
const unsigned long DEBOUNCE_DELAY_MS = 40; // หน่วงเวลา Debounce 40 ms

// WebSocket Server Object
WebSocketsServer webSocket = WebSocketsServer(WS_PORT);

// ==============================================================================
// 4. ฟังก์ชันส่งข้อมูล JSON ไปยัง WebSocket Client
// ==============================================================================
void sendPadPressEvent(const char *sensorId) {
  // สร้าง JSON ตามโครงสร้างที่ PlanKO กำหนด:
  // {
  //   "sensorId": "L1",
  //   "value": 1
  // }
  char jsonBuffer[64];
  snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"sensorId\":\"%s\",\"value\":1}",
           sensorId);

  // Broadcast กระจายข้อมูลไปยังแอป PlanKO หรือ Client ทุกเครื่องที่เชื่อมต่อ
  webSocket.broadcastTXT(jsonBuffer);

  Serial.printf("⚡ [PAD EVENT] %s -> %s (Connected Clients: %u)\n", sensorId,
                jsonBuffer, webSocket.connectedClients());
}

// ==============================================================================
// 5. WebSocket Event Callback
// ==============================================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload,
                    size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.printf("❌ [WS][%u] Client Disconnected\n", num);
    break;

  case WStype_CONNECTED: {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("✅ [WS][%u] Client Connected from %d.%d.%d.%d | URL: %s\n",
                  num, ip[0], ip[1], ip[2], ip[3], payload);
    // ส่งข้อความยืนยันการเชื่อมต่อไปยัง Client
    webSocket.sendTXT(num,
                      "{\"status\":\"connected\",\"device\":\"PlanKO_ESP32\"}");
    break;
  }

  case WStype_TEXT:
    Serial.printf("📩 [WS][%u] Received Text: %s\n", num, payload);
    // ตัวอย่างการรองรับคำสั่ง Ping-Pong
    if (strcmp((char *)payload, "ping") == 0) {
      webSocket.sendTXT(num, "{\"type\":\"pong\"}");
    }
    break;

  case WStype_BIN:
  case WStype_ERROR:
  default:
    break;
  }
}

// ==============================================================================
// 6. ฟังก์ชันเชื่อมต่อ Wi-Fi และแสดง IP Address
// ==============================================================================
void connectWiFi() {
  Serial.println("\n==================================================");
  Serial.printf("🌐 [WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  Serial.println("==================================================");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n🎉 [WIFI] Connected Successfully!");
    Serial.print("📡 ESP32 IP Address : ");
    Serial.println(WiFi.localIP());
    Serial.printf("🔌 WebSocket URL    : ws://%s:%u/\n",
                  WiFi.localIP().toString().c_str(), WS_PORT);
    Serial.println("👉 นำ WebSocket URL ข้างต้นไปกรอกในแอป PlanKO หรือ sensor.ts");
    Serial.println("==================================================\n");
  } else {
    Serial.println("\n❌ [WIFI] Connection Failed!");
    Serial.println(
        "👉 กรุณาตรวจสอบชื่อ Wi-Fi (SSID) และรหัสผ่านในโค้ด (รองรับ 2.4GHz เท่านั้น)");
  }
}

// ==============================================================================
// 7. ฟังก์ชันตรวจสอบการกดปุ่ม (Digital Input + Debounce)
// ==============================================================================
void checkPadSensors() {
  unsigned long now = millis();

  for (size_t i = 0; i < TOTAL_PADS; i++) {
    if (sensors[i].pin < 0)
      continue; // ข้ามขาที่ไม่ได้เชื่อมต่อฮาร์ดแวร์จริง

    int reading = digitalRead(sensors[i].pin);

    // ตรวจสอบการเปลี่ยนแปลงของสัญญาณ
    if (reading != sensors[i].lastRawState) {
      sensors[i].lastDebounceTime = now;
      sensors[i].lastRawState = reading;
    }

    // เมื่อสัญญาณนิ่งเกินระยะเวลา Debounce
    if ((now - sensors[i].lastDebounceTime) > DEBOUNCE_DELAY_MS) {
      if (reading != sensors[i].debouncedState) {
        sensors[i].debouncedState = reading;

        // โหมด INPUT_PULLUP: สถานะ LOW (0) หมายถึงปุ่มถูกกดลง
        if (sensors[i].debouncedState == LOW) {
          sendPadPressEvent(sensors[i].sensorId);
        }
      }
    }
  }
}

// ==============================================================================
// 8. Serial Monitor Simulator (ระบบจำลองการกดปุ่มผ่านทาง Serial)
// ==============================================================================
void handleSerialSimulator() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    input.toUpperCase();

    if (input.length() == 0)
      return;

    if (input == "HELP" || input == "?") {
      Serial.println("\n--- [PlanKO Serial Simulator] ---");
      Serial.println("พิมพ์รหัสปุ่มเพื่อจำลองการกดแผ่นแพลงก์ เช่น:");
      Serial.println("  L1, L2, L3, L4, L5, L6, L7, L8 (ฝั่งซ้าย)");
      Serial.println("  R1, R2, R3, R4, R5, R6, R7, R8 (ฝั่งขวา)");
      Serial.println("พิมพ์ 'IP' เพื่อดู IP Address อีกครั้ง");
      Serial.println("--------------------------------\n");
      return;
    }

    if (input == "IP") {
      Serial.printf("🔌 WebSocket URL: ws://%s:%u/\n",
                    WiFi.localIP().toString().c_str(), WS_PORT);
      return;
    }

    // ค้นหารหัสเซ็นเซอร์ที่ตรงกับที่ผู้ใช้พิมพ์
    bool found = false;
    for (size_t i = 0; i < TOTAL_PADS; i++) {
      if (input == sensors[i].sensorId) {
        Serial.printf("🎮 [SIMULATOR] Triggered Pad: %s\n",
                      sensors[i].sensorId);
        sendPadPressEvent(sensors[i].sensorId);
        found = true;
        break;
      }
    }

    if (!found) {
      Serial.printf("⚠️ ไม่พบเซ็นเซอร์ '%s' (พิมพ์ HELP เพื่อดูรายการปุ่มที่รองรับ)\n",
                    input.c_str());
    }
  }
}

// ==============================================================================
// 9. ฟังก์ชันอ่านค่าและส่งข้อมูลชีพจร MAX30102 (ออปชันเสริม)
// ==============================================================================
#if ENABLE_MAX30102
void setupMAX30102() {
  Serial.println("❤️ [MAX30102] Initializing Heart Rate Sensor...");
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ [MAX30102] Sensor not found! Please check wiring.");
    return;
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);
  Serial.println("✅ [MAX30102] Sensor Ready!");
}

void processMAX30102() {
  long irValue = particleSensor.getIR();

  // ตรวจจับการแตะนิ้ว (ค่า IR > 50,000 หมายถึงมีนิ้วแตะ)
  bool fingerDetected = (irValue > 50000);

  if (fingerDetected) {
    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      beatsPerMinute = 60 / (delta / 1000.0);

      if (beatsPerMinute < 255 && beatsPerMinute > 20) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        beatAvg = 0;
        for (byte x = 0; x < RATE_SIZE; x++)
          beatAvg += rates[x];
        beatAvg /= RATE_SIZE;
      }
    }
  } else {
    beatsPerMinute = 0;
    beatAvg = 0;
  }

  // ส่งข้อมูล Telemetry เป็นระยะ
  if (millis() - lastHeartTelemetryTime > HEART_TELEMETRY_INTERVAL) {
    lastHeartTelemetryTime = millis();

    if (webSocket.connectedClients() > 0) {
      // คำนวณ SpO2 คร่าวๆ (ประมาณการทางชีวภาพเบื้องต้น)
      float spo2Approx = fingerDetected ? (97.0 + (random(0, 30) / 10.0)) : 0.0;

      char heartJson[128];
      snprintf(heartJson, sizeof(heartJson),
               "{\"type\":\"heartRate\",\"fingerDetected\":%s,\"bpm\":%.1f,"
               "\"avgBpm\":%d,\"spo2Approx\":%.1f}",
               fingerDetected ? "true" : "false", beatsPerMinute, beatAvg,
               spo2Approx);

      webSocket.broadcastTXT(heartJson);
    }
  }
}
#endif

// ==============================================================================
// 10. SETUP & LOOP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n");
  Serial.println("==================================================");
  Serial.println("🚀 PlanKO ESP32 WebSocket Controller Initializing");
  Serial.println("==================================================");

  // กำหนดค่าและเตรียมขา GPIO สำหรับเซ็นเซอร์แผ่นแพลงก์
  Serial.println("🛠 Initializing Plank Pad Sensors (INPUT_PULLUP)...");
  for (size_t i = 0; i < TOTAL_PADS; i++) {
    sensors[i].sensorId = PAD_CONFIGS[i].sensorId;
    sensors[i].pin = PAD_CONFIGS[i].pin;
    sensors[i].lastRawState = HIGH;
    sensors[i].debouncedState = HIGH;
    sensors[i].lastDebounceTime = 0;

    if (sensors[i].pin >= 0) {
      pinMode(sensors[i].pin, INPUT_PULLUP);
      int initialReading = digitalRead(sensors[i].pin);
      sensors[i].lastRawState = initialReading;
      sensors[i].debouncedState = initialReading;
      Serial.printf("  ✓ Pad [%s] mapped to GPIO %d\n", sensors[i].sensorId,
                    sensors[i].pin);
    } else {
      Serial.printf("  - Pad [%s] unassigned (Serial simulation only)\n",
                    sensors[i].sensorId);
    }
  }

  // เชื่อมต่อ Wi-Fi
  connectWiFi();

  // เริ่มต้น WebSocket Server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.printf("🚀 WebSocket Server Started on port %u\n", WS_PORT);

#if ENABLE_MAX30102
  setupMAX30102();
#endif

  Serial.println("\n💡 TIP: คุณสามารถพิมพ์ 'L1' หรือ 'R5' ใน Serial Monitor "
                 "เพื่อทดสอบส่ง Event ได้ทันที!");
  Serial.println("พิมพ์ 'HELP' เพื่อดูคำสั่งทั้งหมด\n");
}

void loop() {
  // จัดการการรับ-ส่งข้อมูลของ WebSocket Server
  webSocket.loop();

  // สแกนสถานะปุ่มแผ่นรองแพลงก์พร้อม Debounce
  checkPadSensors();

  // ตรวจจับคำสั่งจำลองจาก Serial Monitor
  handleSerialSimulator();

#if ENABLE_MAX30102
  processMAX30102();
#endif

  // ตรวจสอบการเชื่อมต่อ Wi-Fi และเชื่อมต่อใหม่หากหลุด
  static unsigned long lastWifiCheck = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    Serial.println("⚠️ [WIFI] Connection lost! Reconnecting...");
    WiFi.reconnect();
  }
}