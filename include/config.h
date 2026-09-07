#pragma once

// ============================================================
// Project
// ============================================================

#define PROJECT_NAME "cisone-garage-sentinel"
#define FW_VERSION   "0.2.6"

#define SERIAL_BAUD 115200


// ============================================================
// Sipeed I2S microphone
// ============================================================

#define MIC_I2S_PORT I2S_NUM_0

#define MIC_PIN_SD   21
#define MIC_PIN_SCK  42
#define MIC_PIN_WS   41

#define MIC_SAMPLE_RATE 48000

#define MIC_DMA_BUFFER_COUNT 8
#define MIC_DMA_BUFFER_LENGTH 256

#define MIC_READ_WORDS 512
#define MIC_READ_TIMEOUT_MS 100


// ============================================================
// WAV proof
// ============================================================

#define RECORD_SECONDS 10

// Discard microphone/I2S startup transient.
#define MIC_SETTLE_MS 1000