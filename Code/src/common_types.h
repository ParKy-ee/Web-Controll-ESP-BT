#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <Arduino.h>

// BLE Service & Characteristic UUIDs
#define PLANKO_SERVICE_UUID        "4E6F6465-0001-1000-8000-00805F9B34FB"
#define PLANKO_CHARACTERISTIC_UUID "4E6F6465-0002-1000-8000-00805F9B34FB"

// Compact 20-byte Binary Packet for ultra-fast BLE transmission
// Fits comfortably within standard 23-byte BLE MTU without negotiation
struct __attribute__((packed)) ImuPacket {
    uint8_t  nodeId;       // Node ID (1 - 4)
    uint16_t sequence;     // Packet Sequence number (for packet loss tracking)
    uint32_t timestamp;    // millis() on node
    int16_t  pitch;        // Pitch in degrees * 100
    int16_t  roll;         // Roll in degrees * 100
    int16_t  yaw;          // Yaw in degrees * 100
    int16_t  ax;           // Accel X (m/s^2 * 100)
    int16_t  ay;           // Accel Y (m/s^2 * 100)
    int16_t  az;           // Accel Z (m/s^2 * 100)
    uint8_t  status;       // 1 = Calibrated/OK, 0 = Uncalibrated/Error
};

// Telemetry state stored on Gateway for each node
struct NodeTelemetry {
    volatile bool connected; // volatile: written by BLE callback task, read by main loop
    uint16_t lastSequence;
    uint32_t totalDropped;
    uint32_t lastTimestamp;
    float    pitch;
    float    roll;
    float    yaw;
    float    ax;
    float    ay;
    float    az;
    uint8_t  status;
    volatile unsigned long lastReceivedMillis; // volatile: written by BLE callback task, read by main loop
};

#endif // COMMON_TYPES_H
