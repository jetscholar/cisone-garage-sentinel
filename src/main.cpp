#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>

#include "driver/i2s.h"
#include "config.h"


// ============================================================
// Audio health snapshot
// ============================================================

struct AudioSnapshot
{
    bool valid = false;

    uint32_t windowMs = 0;
    uint32_t lastDataMs = 0;

    uint64_t windowSamples = 0;
    uint64_t totalSamples = 0;

    double measuredSampleRate = 0.0;

    double dcMean = 0.0;
    double rms = 0.0;
    double rmsDbfs = -120.0;

    double peak = 0.0;
    double peakDbfs = -120.0;

    uint32_t readErrors = 0;
    uint32_t readTimeouts = 0;
    uint32_t zeroReads = 0;
};


static AudioSnapshot gAudioSnapshot;

static portMUX_TYPE gAudioSnapshotMux =
    portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t gAudioTaskHandle =
    nullptr;


// Live timestamp updated directly by the audio task.
// Health reporting must not depend on the age of
// the once-per-second analysis snapshot.
static volatile uint32_t gLastAudioDataMs =
    0;


// ============================================================
// PSRAM circular audio buffer
// ============================================================

static int16_t* gAudioRing =
    nullptr;

static size_t gAudioRingCapacitySamples =
    0;

static size_t gAudioRingWriteIndex =
    0;

static uint64_t gAudioRingTotalWritten =
    0;

static uint32_t gAudioRingWraps =
    0;

static bool gAudioRingFull =
    false;


static portMUX_TYPE gAudioRingMux =
    portMUX_INITIALIZER_UNLOCKED;


// ============================================================
// I2S setup
// ============================================================

static bool setupMicrophone()
{
    Serial.println();
    Serial.println(
        "---- Sipeed I2S microphone setup ----"
    );


    const i2s_config_t i2sConfig = {
        .mode = static_cast<i2s_mode_t>(
            I2S_MODE_MASTER |
            I2S_MODE_RX
        ),

        .sample_rate =
            MIC_SAMPLE_RATE,

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_32BIT,

        // Capture both I2S slots.
        // Previous diagnostics established that SLOT A
        // contains the Sipeed microphone data.
        .channel_format =
            I2S_CHANNEL_FMT_RIGHT_LEFT,

        .communication_format =
            I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count =
            MIC_DMA_BUFFER_COUNT,

        .dma_buf_len =
            MIC_DMA_BUFFER_LENGTH,

        .use_apll = false,

        .tx_desc_auto_clear = false,

        .fixed_mclk = 0
    };


    const i2s_pin_config_t pinConfig = {
        .bck_io_num =
            MIC_PIN_SCK,

        .ws_io_num =
            MIC_PIN_WS,

        .data_out_num =
            I2S_PIN_NO_CHANGE,

        .data_in_num =
            MIC_PIN_SD
    };


    esp_err_t result =
        i2s_driver_install(
            MIC_I2S_PORT,
            &i2sConfig,
            0,
            nullptr
        );


    if (
        result != ESP_OK
    )
    {
        Serial.printf(
            "FAIL: i2s_driver_install(): %s\n",
            esp_err_to_name(result)
        );

        return false;
    }


    result =
        i2s_set_pin(
            MIC_I2S_PORT,
            &pinConfig
        );


    if (
        result != ESP_OK
    )
    {
        Serial.printf(
            "FAIL: i2s_set_pin(): %s\n",
            esp_err_to_name(result)
        );

        return false;
    }


    i2s_zero_dma_buffer(
        MIC_I2S_PORT
    );


    Serial.printf(
        "Sample rate : %u Hz\n",
        MIC_SAMPLE_RATE
    );

    Serial.println(
        "I2S slots   : RIGHT_LEFT"
    );

    Serial.println(
        "Active slot : SLOT A"
    );

    Serial.println(
        "Sample      : raw >> 8"
    );

    Serial.printf(
        "SD          : GPIO%d\n",
        MIC_PIN_SD
    );

    Serial.printf(
        "SCK/BCLK    : GPIO%d\n",
        MIC_PIN_SCK
    );

    Serial.printf(
        "WS/LRCLK    : GPIO%d\n",
        MIC_PIN_WS
    );


    return true;
}


// ============================================================
// Microphone startup settling
// ============================================================

static void settleMicrophone()
{
    int32_t buffer[
        MIC_READ_WORDS
    ];


    Serial.printf(
        "Settling microphone for %u ms...\n",
        MIC_SETTLE_MS
    );


    const uint32_t start =
        millis();


    while (
        millis() - start <
        MIC_SETTLE_MS
    )
    {
        size_t bytesRead =
            0;


        i2s_read(
            MIC_I2S_PORT,
            buffer,
            sizeof(buffer),
            &bytesRead,
            pdMS_TO_TICKS(
                MIC_READ_TIMEOUT_MS
            )
        );
    }


    i2s_zero_dma_buffer(
        MIC_I2S_PORT
    );


    Serial.println(
        "Microphone settled."
    );
}


// ============================================================
// PSRAM ring-buffer setup
// ============================================================

static bool setupAudioRing()
{
    gAudioRingCapacitySamples =
        static_cast<size_t>(
            MIC_SAMPLE_RATE
        ) *
        AUDIO_RING_SECONDS;


    const size_t ringBytes =
        gAudioRingCapacitySamples *
        sizeof(int16_t);


    Serial.println();
    Serial.println(
        "---- PSRAM audio ring buffer ----"
    );


    Serial.printf(
        "Duration      : %u seconds\n",
        AUDIO_RING_SECONDS
    );


    Serial.printf(
        "Capacity      : %u samples\n",
        static_cast<unsigned>(
            gAudioRingCapacitySamples
        )
    );


    Serial.printf(
        "Storage       : %u bytes (%.2f KiB)\n",
        static_cast<unsigned>(
            ringBytes
        ),
        static_cast<double>(
            ringBytes
        ) /
        1024.0
    );


    gAudioRing =
        static_cast<int16_t*>(
            heap_caps_malloc(
                ringBytes,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            )
        );


    if (
        gAudioRing ==
        nullptr
    )
    {
        Serial.println(
            "FAIL: PSRAM ring-buffer allocation"
        );

        return false;
    }


    memset(
        gAudioRing,
        0,
        ringBytes
    );


    gAudioRingWriteIndex =
        0;

    gAudioRingTotalWritten =
        0;

    gAudioRingWraps =
        0;

    gAudioRingFull =
        false;


    Serial.println(
        "Ring buffer allocation: PASS"
    );


    Serial.printf(
        "Free PSRAM    : %u bytes\n",
        static_cast<unsigned>(
            ESP.getFreePsram()
        )
    );


    return true;
}


// ============================================================
// Publish completed analysis window
// ============================================================

static void publishSnapshot(
    uint32_t windowMs,
    uint64_t windowSamples,
    uint64_t totalSamples,
    double sum,
    double sumSquares,
    int32_t minSample,
    int32_t maxSample,
    uint32_t lastDataMs,
    uint32_t readErrors,
    uint32_t readTimeouts,
    uint32_t zeroReads
)
{
    if (
        windowSamples ==
        0
    )
    {
        return;
    }


    const double count =
        static_cast<double>(
            windowSamples
        );


    const double mean =
        sum /
        count;


    double variance =
        (
            sumSquares /
            count
        ) -
        (
            mean *
            mean
        );


    if (
        variance <
        0.0
    )
    {
        variance =
            0.0;
    }


    const double rms =
        sqrt(
            variance
        );


    const double minCentered =
        static_cast<double>(
            minSample
        ) -
        mean;


    const double maxCentered =
        static_cast<double>(
            maxSample
        ) -
        mean;


    const double peak =
        fmax(
            fabs(
                minCentered
            ),
            fabs(
                maxCentered
            )
        );


    // Signed 24-bit PCM full scale.
    const double fullScale24 =
        8388607.0;


    const double rmsDbfs =
        rms > 0.0
        ? 20.0 *
          log10(
              rms /
              fullScale24
          )
        : -120.0;


    const double peakDbfs =
        peak > 0.0
        ? 20.0 *
          log10(
              peak /
              fullScale24
          )
        : -120.0;


    const double measuredRate =
        windowMs > 0
        ? (
            static_cast<double>(
                windowSamples
            ) *
            1000.0 /
            static_cast<double>(
                windowMs
            )
        )
        : 0.0;


    AudioSnapshot snapshot;


    snapshot.valid =
        true;

    snapshot.windowMs =
        windowMs;

    snapshot.lastDataMs =
        lastDataMs;

    snapshot.windowSamples =
        windowSamples;

    snapshot.totalSamples =
        totalSamples;

    snapshot.measuredSampleRate =
        measuredRate;

    snapshot.dcMean =
        mean;

    snapshot.rms =
        rms;

    snapshot.rmsDbfs =
        rmsDbfs;

    snapshot.peak =
        peak;

    snapshot.peakDbfs =
        peakDbfs;

    snapshot.readErrors =
        readErrors;

    snapshot.readTimeouts =
        readTimeouts;

    snapshot.zeroReads =
        zeroReads;


    portENTER_CRITICAL(
        &gAudioSnapshotMux
    );

    gAudioSnapshot =
        snapshot;

    portEXIT_CRITICAL(
        &gAudioSnapshotMux
    );
}


// ============================================================
// Continuous audio acquisition task
// ============================================================

static void audioTask(
    void* parameter
)
{
    (void)parameter;


    int32_t buffer[
        MIC_READ_WORDS
    ];


    uint64_t totalSamples =
        0;

    uint64_t windowSamples =
        0;


    double sum =
        0.0;

    double sumSquares =
        0.0;


    int32_t minSample =
        INT32_MAX;

    int32_t maxSample =
        INT32_MIN;


    uint32_t readErrors =
        0;

    uint32_t readTimeouts =
        0;

    uint32_t zeroReads =
        0;


    uint32_t lastDataMs =
        millis();

    uint32_t windowStartMs =
        millis();


    // Keep frequently updated ring state local to the
    // audio task. Only publish metadata periodically.
    size_t ringWriteIndex =
        0;

    uint64_t ringTotalWritten =
        0;

    uint32_t ringWraps =
        0;

    bool ringFull =
        false;


    Serial.printf(
        "Audio task started on Core %d\n",
        xPortGetCoreID()
    );


    while (
        true
    )
    {
        size_t bytesRead =
            0;


        const esp_err_t result =
            i2s_read(
                MIC_I2S_PORT,
                buffer,
                sizeof(buffer),
                &bytesRead,
                pdMS_TO_TICKS(
                    MIC_READ_TIMEOUT_MS
                )
            );


        if (
            result ==
            ESP_ERR_TIMEOUT
        )
        {
            ++readTimeouts;

            continue;
        }


        if (
            result !=
            ESP_OK
        )
        {
            ++readErrors;


            // Avoid a busy loop if an unexpected
            // persistent I2S error occurs.
            vTaskDelay(
                pdMS_TO_TICKS(
                    1
                )
            );


            continue;
        }


        if (
            bytesRead ==
            0
        )
        {
            ++zeroReads;

            continue;
        }


        lastDataMs =
            millis();


        gLastAudioDataMs =
            lastDataMs;


        const size_t wordsRead =
            bytesRead /
            sizeof(int32_t);


        // RIGHT_LEFT produces:
        //
        // buffer[0] = SLOT A
        // buffer[1] = SLOT B
        // buffer[2] = SLOT A
        // buffer[3] = SLOT B
        //
        // SLOT A contains the microphone data.
        for (
            size_t i = 0;
            i + 1 < wordsRead;
            i += 2
        )
        {
            // Convert aligned I2S word to signed
            // 24-bit microphone sample.
            const int32_t sample24 =
                buffer[i] >>
                8;


            // ------------------------------------------------
            // Store rolling PCM16 audio in PSRAM
            // ------------------------------------------------

            int32_t sample16 =
                sample24 >>
                8;


            if (
                sample16 >
                32767
            )
            {
                sample16 =
                    32767;
            }
            else if (
                sample16 <
                -32768
            )
            {
                sample16 =
                    -32768;
            }


            gAudioRing[
                ringWriteIndex
            ] =
                static_cast<int16_t>(
                    sample16
                );


            ++ringWriteIndex;
            ++ringTotalWritten;


            if (
                ringWriteIndex >=
                gAudioRingCapacitySamples
            )
            {
                ringWriteIndex =
                    0;

                ++ringWraps;

                ringFull =
                    true;
            }


            // ------------------------------------------------
            // Existing 24-bit analysis
            // ------------------------------------------------

            const double value =
                static_cast<double>(
                    sample24
                );


            sum +=
                value;


            sumSquares +=
                value *
                value;


            if (
                sample24 <
                minSample
            )
            {
                minSample =
                    sample24;
            }


            if (
                sample24 >
                maxSample
            )
            {
                maxSample =
                    sample24;
            }


            ++windowSamples;
            ++totalSamples;
        }


        // Publish only ring metadata under the critical
        // section. The actual audio writes are never
        // performed while interrupts are locked.
        portENTER_CRITICAL(
            &gAudioRingMux
        );

        gAudioRingWriteIndex =
            ringWriteIndex;

        gAudioRingTotalWritten =
            ringTotalWritten;

        gAudioRingWraps =
            ringWraps;

        gAudioRingFull =
            ringFull;

        portEXIT_CRITICAL(
            &gAudioRingMux
        );


        const uint32_t now =
            millis();


        const uint32_t elapsed =
            now -
            windowStartMs;


        if (
            elapsed >=
            AUDIO_ANALYSIS_WINDOW_MS
        )
        {
            publishSnapshot(
                elapsed,
                windowSamples,
                totalSamples,
                sum,
                sumSquares,
                minSample,
                maxSample,
                lastDataMs,
                readErrors,
                readTimeouts,
                zeroReads
            );


            windowSamples =
                0;

            sum =
                0.0;

            sumSquares =
                0.0;

            minSample =
                INT32_MAX;

            maxSample =
                INT32_MIN;

            windowStartMs =
                now;
        }
    }
}


// ============================================================
// Audio health report
// ============================================================

static void printAudioReport()
{
    AudioSnapshot snapshot;


    portENTER_CRITICAL(
        &gAudioSnapshotMux
    );

    snapshot =
        gAudioSnapshot;

    portEXIT_CRITICAL(
        &gAudioSnapshotMux
    );


    if (
        !snapshot.valid
    )
    {
        Serial.println(
            "[AUDIO] Waiting for first analysis window..."
        );

        return;
    }


    const uint32_t liveLastDataMs =
        gLastAudioDataMs;


    const uint32_t ageMs =
        millis() -
        liveLastDataMs;


    const bool healthy =
        ageMs <=
        AUDIO_HEALTH_TIMEOUT_MS;


    Serial.println();
    Serial.println(
        "---- Audio health ----"
    );


    Serial.printf(
        "Health        : %s\n",
        healthy
            ? "OK"
            : "STALE"
    );


    Serial.printf(
        "Sample rate   : %.1f Hz\n",
        snapshot.measuredSampleRate
    );


    Serial.printf(
        "Window samples: %llu\n",
        static_cast<unsigned long long>(
            snapshot.windowSamples
        )
    );


    Serial.printf(
        "Total samples : %llu\n",
        static_cast<unsigned long long>(
            snapshot.totalSamples
        )
    );


    Serial.printf(
        "DC mean       : %.1f\n",
        snapshot.dcMean
    );


    Serial.printf(
        "RMS           : %.1f\n",
        snapshot.rms
    );


    Serial.printf(
        "RMS level     : %.1f dBFS\n",
        snapshot.rmsDbfs
    );


    Serial.printf(
        "Peak level    : %.1f dBFS\n",
        snapshot.peakDbfs
    );


    Serial.printf(
        "Read errors   : %u\n",
        snapshot.readErrors
    );


    Serial.printf(
        "Timeouts      : %u\n",
        snapshot.readTimeouts
    );


    Serial.printf(
        "Zero reads    : %u\n",
        snapshot.zeroReads
    );


    Serial.printf(
        "Last data age : %u ms\n",
        ageMs
    );
}


// ============================================================
// Ring-buffer report
// ============================================================

static void printRingReport()
{
    size_t writeIndex;
    uint64_t totalWritten;
    uint32_t wraps;
    bool full;


    portENTER_CRITICAL(
        &gAudioRingMux
    );

    writeIndex =
        gAudioRingWriteIndex;

    totalWritten =
        gAudioRingTotalWritten;

    wraps =
        gAudioRingWraps;

    full =
        gAudioRingFull;

    portEXIT_CRITICAL(
        &gAudioRingMux
    );


    const uint64_t retainedSamples =
        totalWritten >=
        gAudioRingCapacitySamples
        ? gAudioRingCapacitySamples
        : totalWritten;


    const double retainedSeconds =
        static_cast<double>(
            retainedSamples
        ) /
        static_cast<double>(
            MIC_SAMPLE_RATE
        );


    const double fillPercent =
        gAudioRingCapacitySamples > 0
        ? (
            100.0 *
            static_cast<double>(
                retainedSamples
            ) /
            static_cast<double>(
                gAudioRingCapacitySamples
            )
        )
        : 0.0;


    Serial.println();
    Serial.println(
        "---- Audio ring buffer ----"
    );


    Serial.printf(
        "Capacity      : %u samples\n",
        static_cast<unsigned>(
            gAudioRingCapacitySamples
        )
    );


    Serial.printf(
        "Retained      : %llu samples\n",
        static_cast<unsigned long long>(
            retainedSamples
        )
    );


    Serial.printf(
        "Retained time : %.2f seconds\n",
        retainedSeconds
    );


    Serial.printf(
        "Fill          : %.1f %%\n",
        fillPercent
    );


    Serial.printf(
        "Write index   : %u\n",
        static_cast<unsigned>(
            writeIndex
        )
    );


    Serial.printf(
        "Wraps         : %u\n",
        wraps
    );


    Serial.printf(
        "Total written : %llu\n",
        static_cast<unsigned long long>(
            totalWritten
        )
    );


    Serial.printf(
        "Full          : %s\n",
        full
            ? "YES"
            : "NO"
    );
}


// ============================================================
// Memory report
// ============================================================

static void printMemoryReport()
{
    Serial.println();
    Serial.println(
        "---- Memory health ----"
    );


    Serial.printf(
        "Free heap     : %u bytes\n",
        static_cast<unsigned>(
            ESP.getFreeHeap()
        )
    );


    Serial.printf(
        "Min free heap : %u bytes\n",
        static_cast<unsigned>(
            ESP.getMinFreeHeap()
        )
    );


    Serial.printf(
        "Free PSRAM    : %u bytes\n",
        static_cast<unsigned>(
            ESP.getFreePsram()
        )
    );


    printRingReport();
}


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(
        SERIAL_BAUD
    );


    delay(
        2000
    );


    Serial.println();
    Serial.println(
        "================================================"
    );

    Serial.println(
        PROJECT_NAME
    );

    Serial.printf(
        "Firmware: %s\n",
        FW_VERSION
    );

    Serial.println(
        "Phase 3 - PSRAM circular audio buffer"
    );

    Serial.println(
        "================================================"
    );


    Serial.printf(
        "Chip  : %s\n",
        ESP.getChipModel()
    );


    Serial.printf(
        "PSRAM : %.2f MiB\n",
        static_cast<float>(
            ESP.getPsramSize()
        ) /
        (
            1024.0f *
            1024.0f
        )
    );


    if (
        !setupMicrophone()
    )
    {
        Serial.println(
            "I2S INITIALIZATION FAILED"
        );


        while (
            true
        )
        {
            delay(
                1000
            );
        }
    }


    settleMicrophone();


    if (
        !setupAudioRing()
    )
    {
        Serial.println(
            "AUDIO RING INITIALIZATION FAILED"
        );


        while (
            true
        )
        {
            delay(
                1000
            );
        }
    }


    const BaseType_t taskResult =
        xTaskCreatePinnedToCore(
            audioTask,
            "audio",
            AUDIO_TASK_STACK_SIZE,
            nullptr,
            AUDIO_TASK_PRIORITY,
            &gAudioTaskHandle,
            AUDIO_TASK_CORE
        );


    if (
        taskResult !=
        pdPASS
    )
    {
        Serial.println(
            "FAIL: could not create audio task"
        );


        while (
            true
        )
        {
            delay(
                1000
            );
        }
    }


    Serial.println();


    Serial.printf(
        "Audio task priority : %d\n",
        AUDIO_TASK_PRIORITY
    );


    Serial.printf(
        "Audio task core     : %d\n",
        AUDIO_TASK_CORE
    );


    Serial.println(
        "Continuous acquisition running."
    );

    Serial.println(
        "Rolling 10-second audio buffer running."
    );
}


// ============================================================
// Main loop
// ============================================================

void loop()
{
    static uint32_t lastAudioReport =
        0;


    static uint32_t lastMemoryReport =
        0;


    const uint32_t now =
        millis();


    if (
        now -
        lastAudioReport >=
        AUDIO_REPORT_INTERVAL_MS
    )
    {
        lastAudioReport =
            now;


        printAudioReport();
    }


    if (
        now -
        lastMemoryReport >=
        MEMORY_REPORT_INTERVAL_MS
    )
    {
        lastMemoryReport =
            now;


        printMemoryReport();
    }


    delay(
        10
    );
}