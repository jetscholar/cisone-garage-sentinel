#pragma once

#define PROJECT_NAME "cisone-garage-sentinel"
#define FW_VERSION "0.1.1"

#define SERIAL_BAUD 115200

#define HEARTBEAT_INTERVAL_MS 5000

// Phase 1 PSRAM verification.
// Test 1 MiB without placing excessive load on the device.
#define PSRAM_TEST_SIZE (1024 * 1024)