#pragma once

// ============================================================
// Project
// ============================================================

#define PROJECT_NAME "cisone-garage-sentinel"
#define FW_VERSION   "0.2.9"

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

#define MIC_SETTLE_MS 1000


// ============================================================
// Phase 3 continuous audio task
// ============================================================

#define AUDIO_TASK_CORE 1
#define AUDIO_TASK_PRIORITY 3
#define AUDIO_TASK_STACK_SIZE 4096

#define AUDIO_ANALYSIS_WINDOW_MS 1000
#define AUDIO_REPORT_INTERVAL_MS 2000
#define MEMORY_REPORT_INTERVAL_MS 10000

#define AUDIO_HEALTH_TIMEOUT_MS 250


// ============================================================
// Phase 3 PSRAM circular audio buffer
// ============================================================

// Retain the most recent 10 seconds of audio.
//
// 48,000 samples/s
// × 10 seconds
// × 2 bytes PCM16
// = 960,000 bytes
#define AUDIO_RING_SECONDS 10