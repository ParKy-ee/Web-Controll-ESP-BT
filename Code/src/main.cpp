/**
 * ==============================================================================
 * Project: PlanKO - Smart Plank Training System
 * Hardware: ESP32-C3 (DevKitM-1 / SuperMini)
 *
 * Supports 2 Unified Roles via PlatformIO build flags:
 *   1. ROLE_GATEWAY:
 *      - BLE Central (connects to up to 4 IMU Nodes via BLE GATT)
 *      - Wi-Fi Station + WebSocket Server on Port 81
 *      - 16-pad Smart Mat Switch Inputs + Debounce + Serial Simulator
 *      - (Optional) MAX30102 Pulse Oximeter
 *      - Broadcasts consolidated IMU + Touch events to Expo App
 *
 *   2. ROLE_NODE:
 *      - BLE Peripheral (Advertises as PlanKO_IMU_1 .. PlanKO_IMU_4)
 *      - Reads MPU-6050 6-DOF IMU over I2C (SDA=4, SCL=5)
 *      - Computes Pitch, Roll, Yaw via Complementary Filter
 *      - Transmits 20-byte binary ImuPacket via BLE Notifications
 * ==============================================================================
 */

#include "common_types.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>

#if defined(ROLE_NODE)
// ==============================================================================
// ------------------------------ ROLE: IMU NODE
// --------------------------------
// ==============================================================================
#include "mpu6050_reader.h"
#include <NimBLEDevice.h>

#ifndef NODE_ID
#define NODE_ID 1
#endif

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 4
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 5
#endif

// Sampling frequency: 50Hz (20ms)
const unsigned long SAMPLE_INTERVAL_MS = 20;

Mpu6050Reader imu;
NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pImuCharacteristic = nullptr;
bool bleConnected = false;
uint16_t packetSequence = 0;
unsigned long lastSampleTime = 0;
unsigned long lastSerialPrintTime = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer) override {
    bleConnected = true;
    Serial.println("🔗 [BLE] Gateway Central Connected!");
  }
  void onDisconnect(NimBLEServer *pServer) override {
    bleConnected = false;
    Serial.println(
        "🔌 [BLE] Gateway Central Disconnected. Restarting Advertising...");
    NimBLEDevice::startAdvertising();
  }
};

void printNodeBanner() {
  Serial.println(
      "\n****************************************************************");
  Serial.printf("🤖 [PlanKO IMU Node #%d] Firmware Started!\n", NODE_ID);
  Serial.printf("📡 BLE Advertising Name: PlanKO_IMU_%d\n", NODE_ID);
  Serial.printf("🔌 I2C Pins: SDA = GPIO %d, SCL = GPIO %d\n", I2C_SDA_PIN,
                I2C_SCL_PIN);
  Serial.println("💡 Type 'CAL' in Serial Monitor to re-calibrate IMU sensor");
  Serial.println(
      "****************************************************************\n");
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(1000);

  printNodeBanner();

  // 1. Initialize MPU-6050 IMU
  Serial.println("🔍 [IMU] Initializing MPU-6050...");
  if (!imu.begin(I2C_SDA_PIN, I2C_SCL_PIN)) {
    Serial.println("❌ [IMU] MPU-6050 not detected. Retrying in background...");
  }

  // 2. Initialize NimBLE Peripheral
  char bleName[20];
  snprintf(bleName, sizeof(bleName), "PlanKO_IMU_%d", NODE_ID);
  NimBLEDevice::init(bleName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum TX power

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(PLANKO_SERVICE_UUID);
  pImuCharacteristic = pService->createCharacteristic(
      PLANKO_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(PLANKO_SERVICE_UUID);
  pAdvertising->setName(bleName);
  pAdvertising->start();

  Serial.printf("🚀 [BLE] Advertising as '%s' - Waiting for Gateway...\n",
                bleName);
}

void loop() {
  unsigned long now = millis();

  // Check for Serial commands (e.g. CAL)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();
    if (cmd == "CAL") {
      imu.calibrate();
    }
  }

  // Read IMU and send BLE Notification
  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;

    float curPitch = 0.0f;
    float curRoll = 0.0f;
    float curYaw = 0.0f;
    float curAx = 0.0f;
    float curAy = 0.0f;
    float curAz = 9.8f;
    uint8_t curStatus = 0;

    if (imu.isDetected) {
      imu.update();
      curPitch = imu.pitch;
      curRoll = imu.roll;
      curYaw = imu.yaw;
      curAx = imu.ax;
      curAy = imu.ay;
      curAz = imu.az;
      curStatus = imu.isCalibrated ? 1 : 0;
    } else {
      // [MOCK SIMULATION] ระบุชัดเจนว่าเป็น Mock (จำลองการสั่นไหวและมุมเอียงของ Plank)
      imu.update(); // Keeps background auto-detect active
      float t = now / 1000.0f;
      curPitch = sinf(t * 1.2f + (NODE_ID * 0.5f)) * 3.5f;
      curRoll = cosf(t * 0.8f + (NODE_ID * 0.3f)) * 2.0f;
      curYaw = sinf(t * 0.2f) * 1.0f;
      curAx = sinf(t * 1.5f) * 0.1f;
      curAy = cosf(t * 1.5f) * 0.1f;
      curAz = 9.8f + sinf(t * 1.2f) * 0.05f;
      curStatus = 2; // 2 = MOCK DATA
    }

    if (bleConnected && pImuCharacteristic != nullptr) {
      ImuPacket pkt;
      pkt.nodeId = NODE_ID;
      pkt.sequence = ++packetSequence;
      pkt.timestamp = now;
      pkt.pitch = (int16_t)(curPitch * 100.0f);
      pkt.roll = (int16_t)(curRoll * 100.0f);
      pkt.yaw = (int16_t)(curYaw * 100.0f);
      pkt.ax = (int16_t)(curAx * 100.0f);
      pkt.ay = (int16_t)(curAy * 100.0f);
      pkt.az = (int16_t)(curAz * 100.0f);
      pkt.status = curStatus;

      pImuCharacteristic->setValue((uint8_t *)&pkt, sizeof(pkt));
      pImuCharacteristic->notify();
    }
  }

  // Print telemetry to Serial once every 2 seconds
  if (now - lastSerialPrintTime >= 2000) {
    lastSerialPrintTime = now;
    float displayPitch =
        imu.isDetected ? imu.pitch
                       : (sinf(now / 1000.0f * 1.2f + (NODE_ID * 0.5f)) * 3.5f);
    float displayRoll =
        imu.isDetected ? imu.roll
                       : (cosf(now / 1000.0f * 0.8f + (NODE_ID * 0.3f)) * 2.0f);
    Serial.printf(
        "📊 [Node #%d]%s Pitch: %5.1f° | Roll: %5.1f° | BLE: %s | Seq: %u\n",
        NODE_ID, imu.isDetected ? " [REAL IMU]" : " [MOCK SIMULATOR]",
        displayPitch, displayRoll, bleConnected ? "CONNECTED" : "ADVERTISING",
        packetSequence);
  }
}

#elif defined(ROLE_GATEWAY)
// ==============================================================================
// ----------------------------- ROLE: GATEWAY
// ----------------------------------
// ==============================================================================
#include <NimBLEDevice.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Wi-Fi Configuration
const char *WIFI_SSID = "APISIT164 @2.4G";
const char *WIFI_PASS = "09E2898E";
const uint16_t WS_PORT = 81;

// Static IP Configuration
IPAddress STATIC_IP(192, 168, 1, 200);
IPAddress GATEWAY_IP(192, 168, 1, 1);
IPAddress SUBNET_MASK(255, 255, 255, 0);
IPAddress DNS_PRIMARY(192, 168, 1, 1);
IPAddress DNS_SECONDARY(8, 8, 8, 8);

// 16-Pad GPIO Hardware Configuration (ESP32-C3 Safe Pins)
struct PadConfig {
  const char *name;
  int8_t pin;
};

const PadConfig PAD_CONFIGS[] = {
    {"L1", 0},  {"L2", 1},  {"L3", 2},  {"L4", 3},  {"L5", 4},  {"L6", 5},
    {"L7", 6},  {"L8", 7},  {"R1", 10}, {"R2", -1}, {"R3", -1}, {"R4", -1},
    {"R5", -1}, {"R6", -1}, {"R7", -1}, {"R8", -1}};

const size_t TOTAL_PADS = sizeof(PAD_CONFIGS) / sizeof(PAD_CONFIGS[0]);

struct PadSensorState {
  const char *sensorId;
  int8_t pin;
  int lastRawState;
  int debouncedState;
  unsigned long lastDebounceTime;
};

PadSensorState sensors[TOTAL_PADS];
const unsigned long DEBOUNCE_DELAY_MS = 40;

// WebSocket Server
WebSocketsServer webSocket = WebSocketsServer(WS_PORT);

// BLE Central State for 4 IMU Nodes
const int MAX_NODES = 4;
NodeTelemetry nodesTelemetry[MAX_NODES];
NimBLEClient *pNodeClients[MAX_NODES] = {nullptr, nullptr, nullptr, nullptr};
NimBLEAdvertisedDevice *pendingConnectDevices[MAX_NODES] = {nullptr, nullptr,
                                                            nullptr, nullptr};

unsigned long lastWsStreamTime = 0;
const unsigned long WS_STREAM_INTERVAL_MS = 50; // 20Hz WebSocket Broadcast
unsigned long lastBleScanTime = 0;
const unsigned long STALE_TIMEOUT_MS = 3000; // Force disconnect if no data for 3s
bool isScanning = false;

// Forward declaration
void sendPadPressEvent(const char *sensorId);
void broadcastImuStream();

// BLE Client Callbacks
class GatewayClientCallbacks : public NimBLEClientCallbacks {
public:
  uint8_t nodeId;
  GatewayClientCallbacks(uint8_t id) : nodeId(id) {}

  void onConnect(NimBLEClient *pClient) override {
    Serial.printf("✅ [GATEWAY-BLE] Connected to IMU Node #%d\n", nodeId);
    nodesTelemetry[nodeId - 1].connected = true;
  }

  void onDisconnect(NimBLEClient *pClient) override {
    Serial.printf("❌ [GATEWAY-BLE] Disconnected from IMU Node #%d\n", nodeId);
    nodesTelemetry[nodeId - 1].connected = false;
  }
};

// BLE Notification Callback for receiving ImuPacket from Nodes
void notifyCallback(NimBLERemoteCharacteristic *pRemoteChar, uint8_t *pData,
                    size_t length, bool isNotify) {
  if (length < sizeof(ImuPacket))
    return;

  const ImuPacket *pkt = (const ImuPacket *)pData;
  if (pkt->nodeId < 1 || pkt->nodeId > MAX_NODES)
    return;

  int idx = pkt->nodeId - 1;
  NodeTelemetry &telemetry = nodesTelemetry[idx];

  // Check packet drop sequence
  if (telemetry.lastSequence != 0 &&
      pkt->sequence > telemetry.lastSequence + 1) {
    uint16_t dropped = (pkt->sequence - telemetry.lastSequence - 1);
    telemetry.totalDropped += dropped;
    Serial.printf(
        "⚠️ [GATEWAY-BLE] Node #%d Dropped %u packets (Seq: %u -> %u)\n",
        pkt->nodeId, dropped, telemetry.lastSequence, pkt->sequence);
  }
  telemetry.lastSequence = pkt->sequence;
  telemetry.lastTimestamp = pkt->timestamp;
  telemetry.pitch = pkt->pitch / 100.0f;
  telemetry.roll = pkt->roll / 100.0f;
  telemetry.yaw = pkt->yaw / 100.0f;
  telemetry.ax = pkt->ax / 100.0f;
  telemetry.ay = pkt->ay / 100.0f;
  telemetry.az = pkt->az / 100.0f;
  telemetry.status = pkt->status;
  telemetry.lastReceivedMillis = millis();

  // Log live telemetry from node (rate-limited to 1s per node)
  static unsigned long lastNodeLogTime[MAX_NODES] = {0, 0, 0, 0};
  if (millis() - lastNodeLogTime[idx] >= 1000) {
    lastNodeLogTime[idx] = millis();
    Serial.printf("📥 [GW-RECV] Node #%d%s | Seq: %5u | Pitch: %5.1f° | Roll: "
                  "%5.1f° | Yaw: %5.1f° | Drops: %u\n",
                  pkt->nodeId, (pkt->status == 2) ? " [MOCK]" : " [REAL]",
                  pkt->sequence, telemetry.pitch, telemetry.roll, telemetry.yaw,
                  telemetry.totalDropped);
  }
}

// Advertised Device Callback
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
    std::string name = advertisedDevice->getName();
    if (name.rfind("PlanKO_IMU_", 0) == 0) { // Name starts with PlanKO_IMU_
      int id = name[11] - '0';
      if (id >= 1 && id <= MAX_NODES) {
        int idx = id - 1;
        if (!nodesTelemetry[idx].connected &&
            pendingConnectDevices[idx] == nullptr) {
          Serial.printf("🔎 [GATEWAY-BLE] Found Node #%d ('%s') RSSI: %d\n", id,
                        name.c_str(), advertisedDevice->getRSSI());
          pendingConnectDevices[idx] =
              new NimBLEAdvertisedDevice(*advertisedDevice);
        }
      }
    }
  }
};

void resumeBleScanIfNeeded() {
  bool needMoreNodes = false;
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodesTelemetry[i].connected)
      needMoreNodes = true;
  }
  NimBLEScan *pScan = NimBLEDevice::getScan();
  if (needMoreNodes && pScan && !pScan->isScanning()) {
    pScan->start(0, false);
  }
}

void connectToNode(int idx) {
  int id = idx + 1;
  NimBLEAdvertisedDevice *advDevice = pendingConnectDevices[idx];
  if (advDevice == nullptr)
    return;

  Serial.printf("⏳ [GATEWAY-BLE] Connecting to Node #%d...\n", id);

  // Stop scanning before connecting to avoid radio contention
  NimBLEScan *pScan = NimBLEDevice::getScan();
  if (pScan && pScan->isScanning()) {
    pScan->stop();
  }

  NimBLEClient *pClient = pNodeClients[idx];
  if (pClient == nullptr) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new GatewayClientCallbacks(id), false);
    pClient->setConnectionParams(12, 12, 0, 150); // Low latency connection
    pClient->setConnectTimeout(5);
    pNodeClients[idx] = pClient;
  }

  if (!pClient->isConnected()) {
    if (!pClient->connect(advDevice)) {
      Serial.printf(
          "❌ [GATEWAY-BLE] Failed to connect to Node #%d (will retry)\n", id);
      delete advDevice;
      pendingConnectDevices[idx] = nullptr;
      resumeBleScanIfNeeded();
      return;
    }
  }

  NimBLERemoteService *pRemoteService =
      pClient->getService(PLANKO_SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.printf("❌ [GATEWAY-BLE] Failed to find service on Node #%d\n", id);
    pClient->disconnect();
    delete advDevice;
    pendingConnectDevices[idx] = nullptr;
    resumeBleScanIfNeeded();
    return;
  }

  NimBLERemoteCharacteristic *pRemoteChar =
      pRemoteService->getCharacteristic(PLANKO_CHARACTERISTIC_UUID);
  if (pRemoteChar == nullptr || !pRemoteChar->canNotify()) {
    Serial.printf(
        "❌ [GATEWAY-BLE] Failed to find notify characteristic on Node #%d\n",
        id);
    pClient->disconnect();
    delete advDevice;
    pendingConnectDevices[idx] = nullptr;
    resumeBleScanIfNeeded();
    return;
  }

  if (!pRemoteChar->subscribe(true, notifyCallback)) {
    Serial.printf(
        "❌ [GATEWAY-BLE] Failed to subscribe to Node #%d notifications\n", id);
    pClient->disconnect();
    delete advDevice;
    pendingConnectDevices[idx] = nullptr;
    resumeBleScanIfNeeded();
    return;
  }

  Serial.printf(
      "🎉 [GATEWAY-BLE] Successfully subscribed to Node #%d telemetry!\n", id);
  nodesTelemetry[idx].connected = true;
  delete advDevice;
  pendingConnectDevices[idx] = nullptr;

  resumeBleScanIfNeeded();
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload,
                    size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.printf("❌ [WS][%u] Client Disconnected\n", num);
    break;
  case WStype_CONNECTED: {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("✅ [WS][%u] Client Connected from %d.%d.%d.%d\n", num, ip[0],
                  ip[1], ip[2], ip[3]);
    webSocket.sendTXT(
        num, "{\"status\":\"connected\",\"device\":\"PlanKO_Gateway\"}");
    break;
  }
  case WStype_TEXT:
    // Echo / Command handling
    break;
  default:
    break;
  }
}

void sendPadPressEvent(const char *sensorId) {
  char jsonBuffer[64];
  snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"sensorId\":\"%s\",\"value\":1}",
           sensorId);
  webSocket.broadcastTXT(jsonBuffer);
  Serial.printf("⚡ [PAD EVENT] %s (WS Clients: %u)\n", sensorId,
                webSocket.connectedClients());
}

void broadcastImuStream() {
  if (webSocket.connectedClients() == 0)
    return;

  unsigned long now = millis();

  JsonDocument doc;
  doc["type"] = "imu_stream";

  JsonObject gw = doc["gateway"].to<JsonObject>();
  gw["uptimeMs"] = now;

  int connectedCount = 0;
  JsonArray nodes = doc["nodes"].to<JsonArray>();
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodesTelemetry[i].connected || nodesTelemetry[i].lastReceivedMillis == 0)
      continue;

    // Snapshot to avoid race with BLE notification callback (separate FreeRTOS task)
    unsigned long lastRecv = nodesTelemetry[i].lastReceivedMillis;
    unsigned long elapsed = now - lastRecv;

    // Guard: underflow produces ~4 billion, real stale is < 60s.
    // Only include nodes with fresh data (received within STALE_TIMEOUT_MS)
    if (elapsed > STALE_TIMEOUT_MS && elapsed < 60000UL)
      continue; // Data is stale, skip this node

    connectedCount++;
    JsonObject n = nodes.add<JsonObject>();
    n["nodeId"] = i + 1;
    n["seq"] = nodesTelemetry[i].lastSequence;
    n["ts"] = nodesTelemetry[i].lastTimestamp;
    n["pitch"] = round(nodesTelemetry[i].pitch * 10.0f) / 10.0f;
    n["roll"] = round(nodesTelemetry[i].roll * 10.0f) / 10.0f;
    n["yaw"] = round(nodesTelemetry[i].yaw * 10.0f) / 10.0f;
    n["dropped"] = nodesTelemetry[i].totalDropped;
    n["isMock"] = (nodesTelemetry[i].status == 2);

    JsonArray acc = n["accel"].to<JsonArray>();
    acc.add(round(nodesTelemetry[i].ax * 100.0f) / 100.0f);
    acc.add(round(nodesTelemetry[i].ay * 100.0f) / 100.0f);
    acc.add(round(nodesTelemetry[i].az * 100.0f) / 100.0f);
  }

  gw["connectedNodes"] = connectedCount;

  String output;
  serializeJson(doc, output);
  webSocket.broadcastTXT(output);
}

void handlePadInputs() {
  unsigned long now = millis();
  for (size_t i = 0; i < TOTAL_PADS; i++) {
    if (sensors[i].pin < 0)
      continue;

    int rawReading = digitalRead(sensors[i].pin);
    if (rawReading != sensors[i].lastRawState) {
      sensors[i].lastDebounceTime = now;
    }

    if ((now - sensors[i].lastDebounceTime) > DEBOUNCE_DELAY_MS) {
      if (rawReading != sensors[i].debouncedState) {
        sensors[i].debouncedState = rawReading;
        if (sensors[i].debouncedState == LOW) {
          sendPadPressEvent(sensors[i].sensorId);
        }
      }
    }
    sensors[i].lastRawState = rawReading;
  }
}

void handleSerialSimulator() {
  if (!Serial.available())
    return;
  String input = Serial.readStringUntil('\n');
  input.trim();
  input.toUpperCase();
  if (input.length() == 0)
    return;

  if (input == "HELP") {
    Serial.println("\n--- [PlanKO Gateway Simulator] ---");
    Serial.println("Type 'L1' - 'L8' or 'R1' - 'R8' to trigger pad event");
    Serial.println("Type 'STATUS' to see connected BLE nodes & stats");
    return;
  }

  if (input == "STATUS") {
    Serial.println("\n--- [Gateway Telemetry Status] ---");
    for (int i = 0; i < MAX_NODES; i++) {
      Serial.printf(
          "  Node #%d: %s | Seq: %u | Dropped: %u | P:%.1f R:%.1f Y:%.1f\n",
          i + 1, nodesTelemetry[i].connected ? "CONNECTED" : "OFFLINE",
          nodesTelemetry[i].lastSequence, nodesTelemetry[i].totalDropped,
          nodesTelemetry[i].pitch, nodesTelemetry[i].roll,
          nodesTelemetry[i].yaw);
    }
    Serial.printf("  WS Clients: %u | Gateway IP: %s\n\n",
                  webSocket.connectedClients(),
                  WiFi.localIP().toString().c_str());
    return;
  }

  for (size_t i = 0; i < TOTAL_PADS; i++) {
    if (input == PAD_CONFIGS[i].name) {
      Serial.printf("🎮 [SIMULATOR] Triggered: %s\n", input.c_str());
      sendPadPressEvent(PAD_CONFIGS[i].name);
      return;
    }
  }
}

void setup() {
  Serial.begin(115200);
  // Give USB CDC time to attach
  for (int i = 0; i < 20 && !Serial; i++) {
    delay(100);
  }
  delay(500);

  Serial.println(
      "\n****************************************************************");
  Serial.println("🌐 [PlanKO GATEWAY] Firmware Initializing...");
  Serial.println(
      "****************************************************************");

  // Initialize Pads
  for (size_t i = 0; i < TOTAL_PADS; i++) {
    sensors[i].sensorId = PAD_CONFIGS[i].name;
    sensors[i].pin = PAD_CONFIGS[i].pin;
    sensors[i].lastRawState = HIGH;
    sensors[i].debouncedState = HIGH;
    sensors[i].lastDebounceTime = 0;
    if (sensors[i].pin >= 0) {
      pinMode(sensors[i].pin, INPUT_PULLUP);
    }
  }

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("\n📡 [WIFI] Associated with Wi-Fi AP!");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("\n🎉 [WIFI] Connected! IP: %s\n",
                    WiFi.localIP().toString().c_str());
      Serial.printf("🔌 WebSocket Server: ws://%s:%u/\n",
                    WiFi.localIP().toString().c_str(), WS_PORT);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      Serial.printf("\n⚠️ [WIFI] Disconnected (Reason: %d). Reconnecting...\n",
                    info.wifi_sta_disconnected.reason);
      NimBLEScan *s = NimBLEDevice::getScan();
      if (s && s->isScanning()) {
        s->stop(); // Pause BLE scan so Wi-Fi can reconnect without radio
                   // interference
      }
      WiFi.reconnect();
      break;
    }
    default:
      break;
    }
  });

  Serial.printf("⏳ [WIFI] Connecting to SSID: '%s' with Static IP: %s...\n",
                WIFI_SSID, STATIC_IP.toString().c_str());
  WiFi.config(STATIC_IP, GATEWAY_IP, SUBNET_MASK, DNS_PRIMARY, DNS_SECONDARY);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts % 20 == 0) {
      Serial.printf("\n⏳ Still connecting to '%s' (Attempt %d)...\n",
                    WIFI_SSID, attempts);
      WiFi.disconnect();
      delay(200);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
  Serial.println();

  Serial.printf("🎉 Wi-Fi Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("🔌 WebSocket URL: ws://%s:%u/\n",
                WiFi.localIP().toString().c_str(), WS_PORT);

  // Start WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.printf("🚀 WebSocket Server listening on port %u\n", WS_PORT);
  Serial.printf("💾 [MEMORY] Free Heap: %u bytes\n", ESP.getFreeHeap());

  // Enable modem sleep AFTER Wi-Fi is ready (satisfies ESP-IDF coexistence
  // requirement)
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  // Initialize BLE Central
  NimBLEDevice::init("PlanKO_Gateway");
  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), false);
  pScan->setActiveScan(true);
  pScan->setDuplicateFilter(
      false); // CRITICAL: Allow finding nodes again on retry/reconnect
  // Balanced duty cycle (160ms interval, 40ms window = 25% duty cycle) leaves
  // 75% radio time for Wi-Fi & WebSockets
  pScan->setInterval(160);
  pScan->setWindow(40);
  pScan->start(0, false); // Continuous scanning in background
  isScanning = true;
  Serial.println(
      "🔎 BLE Central Scanner started. Scanning for PlanKO_IMU_1..4...\n");
}

void loop() {
  webSocket.loop();
  handlePadInputs();
  handleSerialSimulator();

  unsigned long now = millis();

  // Check for pending BLE connections
  for (int i = 0; i < MAX_NODES; i++) {
    if (pendingConnectDevices[i] != nullptr) {
      connectToNode(i);
    }
  }

  // Detect stale BLE nodes (no data received for 3s) and force disconnect
  // Re-read millis() because connectToNode() above is blocking and can take
  // several seconds, making the earlier 'now' stale and causing underflow
  unsigned long freshNow = millis();
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodesTelemetry[i].connected && nodesTelemetry[i].lastReceivedMillis > 0) {
      // Snapshot to avoid race with BLE notification callback (separate FreeRTOS task)
      unsigned long lastRecv = nodesTelemetry[i].lastReceivedMillis;
      unsigned long elapsed = freshNow - lastRecv;
      // Guard: if elapsed wrapped (underflow from callback preemption), skip.
      // Any real stale value will be < 60s; underflow produces ~4 billion.
      if (elapsed > STALE_TIMEOUT_MS && elapsed < 60000UL) {
        Serial.printf(
            "⚠️ [GATEWAY-BLE] Node #%d stale (no data for %lums). Force "
            "disconnecting...\n",
            i + 1, elapsed);
        if (pNodeClients[i] != nullptr && pNodeClients[i]->isConnected()) {
          pNodeClients[i]->disconnect();
        }
        nodesTelemetry[i].connected = false;
        nodesTelemetry[i].lastReceivedMillis = 0;
        nodesTelemetry[i].lastSequence = 0;
        resumeBleScanIfNeeded();
      }
    }
  }

  // Periodic WebSocket Broadcast of consolidated IMU streams (20Hz)
  if (now - lastWsStreamTime >= WS_STREAM_INTERVAL_MS) {
    lastWsStreamTime = now;
    broadcastImuStream();
  }

  // 🌟 Periodic Gateway Heartbeat to Serial every 2 seconds
  static unsigned long lastGatewayHeartbeat = 0;
  if (now - lastGatewayHeartbeat >= 2000) {
    lastGatewayHeartbeat = now;
    int connectedCount = 0;
    char nodesSummary[64] = "";
    for (int i = 0; i < MAX_NODES; i++) {
      if (nodesTelemetry[i].connected) {
        connectedCount++;
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "#%d ", i + 1);
        strcat(nodesSummary, tmp);
      }
    }
    if (connectedCount == 0)
      strcpy(nodesSummary, "None");

    Serial.printf("🌐 [GATEWAY] Wi-Fi: %s (%s) | WS: %u | BLE: %d/4 [%s] | "
                  "Heap: %uB | Uptime: %lus\n",
                  (WiFi.status() == WL_CONNECTED) ? "ONLINE" : "OFFLINE",
                  (WiFi.status() == WL_CONNECTED)
                      ? WiFi.localIP().toString().c_str()
                      : "No IP",
                  webSocket.connectedClients(), connectedCount, nodesSummary,
                  ESP.getFreeHeap(), now / 1000);
  }
}

#else
#error                                                                         \
    "Please select an environment: 'gateway' or 'imu_node_1'..'imu_node_4' in platformio.ini"
#endif