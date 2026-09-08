#include <Arduino.h>
#include <math.h>

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

static volatile uint32_t gLastAudioDataMs =
	0;

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

        // Capture both slots.
        // SLOT A is the microphone channel.
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


    if (result != ESP_OK)
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


    if (result != ESP_OK)
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
// Startup settling
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


    // Signed 24-bit full scale.
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

            // Prevent an unexpected persistent
            // error from becoming a busy loop.
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


        // RIGHT_LEFT gives:
        //
        // [0] SLOT A
        // [1] SLOT B
        // [2] SLOT A
        // [3] SLOT B
        //
        // SLOT A is the Sipeed microphone.
        for (
            size_t i = 0;
            i + 1 < wordsRead;
            i += 2
        )
        {
            const int32_t sample =
                buffer[i] >>
                8;


            const double value =
                static_cast<double>(
                    sample
                );


            sum +=
                value;


            sumSquares +=
                value *
                value;


            if (
                sample <
                minSample
            )
            {
                minSample =
                    sample;
            }


            if (
                sample >
                maxSample
            )
            {
                maxSample =
                    sample;
            }


            ++windowSamples;
            ++totalSamples;
        }


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
// Audio report
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
        "Phase 3 - Continuous audio acquisition"
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