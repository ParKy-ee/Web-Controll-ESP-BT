// สำหรับใช้ Bluetuth Connet หน้า page ทำงานร่วมกับ index.html

// #include <Arduino.h>
// #include <BLEDevice.h>
// #include <BLEUtils.h>
// #include <BLEServer.h>

// #define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
// #define CHARACTERISTIC_UUID "abcd1234-5678-1234-1234-abcdef123456"

// BLECharacteristic *pCharacteristic;

// class MyCallbacks : public BLECharacteristicCallbacks {
//   void onWrite(BLECharacteristic *pChar) {
//     std::string value = pChar->getValue();

//     if (value == "ON") {
//       digitalWrite(2, HIGH);
//       Serial.println("LED ON");
//     } 
//     else if (value == "OFF") {
//       digitalWrite(2, LOW);
//       Serial.println("LED OFF");
//     }
//   }
// };

// void setup() {
//   Serial.begin(115200);
//   pinMode(2, OUTPUT);

//   BLEDevice::init("ESP32_LED");

//   BLEServer *pServer = BLEDevice::createServer();
//   BLEService *pService = pServer->createService(SERVICE_UUID);

//   pCharacteristic = pService->createCharacteristic(
//     CHARACTERISTIC_UUID,
//     BLECharacteristic::PROPERTY_WRITE
//   );

//   pCharacteristic->setCallbacks(new MyCallbacks());

//   pService->start();

//   BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
//   pAdvertising->addServiceUUID(SERVICE_UUID);
//   pAdvertising->start();

//   Serial.println("BLE Ready");
// }

// void loop() {}

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

String ssid = "";
String password = "";

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"

void connectWiFi();

class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    Serial.println("📩 [BLE] onWrite triggered");

    std::string raw = pCharacteristic->getValue();

    if (raw.length() == 0) {
      Serial.println("⚠️ [BLE] Empty value");
      return;
    }

    String value = String(raw.c_str());

    Serial.println("📥 [BLE] Received raw:");
    Serial.println(value);

    int splitIndex = value.indexOf(',');

    if (splitIndex == -1) {
      Serial.println("❌ [ERROR] Format wrong (no comma)");
      return;
    }

    ssid = value.substring(0, splitIndex);
    password = value.substring(splitIndex + 1);

    Serial.println("📶 SSID: " + ssid);
    Serial.println("🔑 PASS: " + password);

    connectWiFi();
  }
};

void connectWiFi() {
  Serial.println("🌐 [WIFI] Connecting to: " + ssid);

  WiFi.begin(ssid.c_str(), password.c_str());

  int retry = 0;

  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  Serial.println();

  wl_status_t status = WiFi.status();

  Serial.print("📡 Status = ");
  Serial.println(status);

  if (status == WL_CONNECTED) {
    Serial.println("✅ Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("❌ Failed");

    switch (status) {
      case WL_NO_SSID_AVAIL:
        Serial.println("👉 หา WiFi ไม่เจอ");
        break;
      case WL_CONNECT_FAILED:
        Serial.println("👉 รหัสผิด");
        break;
      case WL_DISCONNECTED:
        Serial.println("👉 หลุดการเชื่อมต่อ");
        break;
      default:
        Serial.println("👉 error อื่น");
        break;
    }
  }
}
void setup() {
  Serial.begin(115200);
  Serial.println("🚀 ESP32 Started");

  BLEDevice::init("ESP32_Config");
  BLEServer *pServer = BLEDevice::createServer();

  Serial.println("📡 [BLE] Server created");

  BLEService *pService = pServer->createService(SERVICE_UUID);
  Serial.println("📡 [BLE] Service created");

  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );

  pCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();
  Serial.println("📡 [BLE] Service started");

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->start();

  Serial.println("📡 [BLE] Advertising started");
  Serial.println("⏳ Waiting for client...");
}

void loop() {}