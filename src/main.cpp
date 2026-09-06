#include <Arduino.h>
#include <math.h>

#include "driver/i2s.h"
#include "config.h"


// ============================================================
// Per-channel accumulated statistics
// ============================================================

struct ChannelStats
{
    uint64_t samples = 0;

    int32_t minSample = INT32_MAX;
    int32_t maxSample = INT32_MIN;

    int64_t peak = 0;

    long double sum = 0.0;
    long double sumSquares = 0.0;

    uint64_t low7Zero = 0;
    uint64_t low8Zero = 0;
};


static void resetStats(ChannelStats& s)
{
    s.samples = 0;

    s.minSample = INT32_MAX;
    s.maxSample = INT32_MIN;

    s.peak = 0;

    s.sum = 0.0;
    s.sumSquares = 0.0;

    s.low7Zero = 0;
    s.low8Zero = 0;
}


// ============================================================
// Add raw 32-bit word
// ============================================================

static void addSample(
    ChannelStats& stats,
    int32_t raw
)
{
    if (raw < stats.minSample)
    {
        stats.minSample = raw;
    }

    if (raw > stats.maxSample)
    {
        stats.maxSample = raw;
    }


    int64_t magnitude =
        static_cast<int64_t>(raw);

    if (magnitude < 0)
    {
        magnitude = -magnitude;
    }

    if (magnitude > stats.peak)
    {
        stats.peak = magnitude;
    }


    stats.sum +=
        static_cast<long double>(raw);

    stats.sumSquares +=
        static_cast<long double>(raw) *
        static_cast<long double>(raw);


    const uint32_t u =
        static_cast<uint32_t>(raw);

    if ((u & 0x7F) == 0)
    {
        ++stats.low7Zero;
    }

    if ((u & 0xFF) == 0)
    {
        ++stats.low8Zero;
    }


    ++stats.samples;
}


// ============================================================
// Print one channel
// ============================================================

static void printStats(
    const char* name,
    const ChannelStats& stats
)
{
    if (stats.samples == 0)
    {
        Serial.printf(
            "%s: no samples\n",
            name
        );

        return;
    }


    const long double count =
        static_cast<long double>(
            stats.samples
        );


    const double mean =
        static_cast<double>(
            stats.sum / count
        );


    const double rms =
        sqrt(
            static_cast<double>(
                stats.sumSquares / count
            )
        );


    const double low7Percent =
        100.0 *
        static_cast<double>(
            stats.low7Zero
        ) /
        static_cast<double>(
            stats.samples
        );


    const double low8Percent =
        100.0 *
        static_cast<double>(
            stats.low8Zero
        ) /
        static_cast<double>(
            stats.samples
        );


    Serial.printf(
        "%s\n",
        name
    );

    Serial.printf(
        "  samples       : %llu\n",
        static_cast<unsigned long long>(
            stats.samples
        )
    );

    Serial.printf(
        "  raw range     : %ld .. %ld\n",
        static_cast<long>(
            stats.minSample
        ),
        static_cast<long>(
            stats.maxSample
        )
    );

    Serial.printf(
        "  mean/DC       : %.1f\n",
        mean
    );

    Serial.printf(
        "  RMS           : %.1f\n",
        rms
    );

    Serial.printf(
        "  absolute peak : %lld\n",
        static_cast<long long>(
            stats.peak
        )
    );

    Serial.printf(
        "  low 7 zero    : %.1f %%\n",
        low7Percent
    );

    Serial.printf(
        "  low 8 zero    : %.1f %%\n",
        low8Percent
    );
}


// ============================================================
// I2S setup
// ============================================================

static bool setupMicrophone()
{
    Serial.println();
    Serial.println(
        "---- ICS-43434 I2S setup ----"
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

        // Capture both slots so we do not assume
        // the SEL polarity or ESP32 slot ordering.
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
        "Word size   : 32 bit"
    );

    Serial.println(
        "Slot format : RIGHT_LEFT"
    );

    Serial.println(
        "Processing  : raw / none"
    );

    Serial.printf(
        "DOUT/SD     : GPIO%d\n",
        MIC_PIN_SD
    );

    Serial.printf(
        "BCLK        : GPIO%d\n",
        MIC_PIN_SCK
    );

    Serial.printf(
        "LRCL/WS     : GPIO%d\n",
        MIC_PIN_WS
    );


    return true;
}


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(
        SERIAL_BAUD
    );

    delay(2000);


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
        "Phase 2.4 - ICS-43434 control test"
    );

    Serial.println(
        "================================================"
    );


    Serial.printf(
        "Chip : %s\n",
        ESP.getChipModel()
    );

    Serial.printf(
        "PSRAM: %.2f MiB\n",
        static_cast<float>(
            ESP.getPsramSize()
        ) /
        (1024.0f * 1024.0f)
    );


    if (!setupMicrophone())
    {
        Serial.println();
        Serial.println(
            "I2S INITIALIZATION FAILED"
        );

        while (true)
        {
            delay(1000);
        }
    }


    Serial.println();
    Serial.println(
        "ICS-43434 SEL currently connected to GND."
    );

    Serial.println(
        "Comparing both I2S slots."
    );

    Serial.println();
}


// ============================================================
// Loop
// ============================================================

void loop()
{
    static int32_t buffer[
        MIC_READ_WORDS
    ];


    static ChannelStats slotA;
    static ChannelStats slotB;


    static uint32_t windowStart =
        millis();


    size_t bytesRead = 0;


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


    if (result != ESP_OK)
    {
        Serial.printf(
            "I2S read error: %s\n",
            esp_err_to_name(result)
        );

        return;
    }


    const size_t wordCount =
        bytesRead /
        sizeof(int32_t);


    // RIGHT_LEFT mode returns interleaved words.
    //
    // We intentionally call them SLOT A and SLOT B
    // rather than assuming left/right ordering yet.
    for (
        size_t i = 0;
        i + 1 < wordCount;
        i += 2
    )
    {
        addSample(
            slotA,
            buffer[i]
        );

        addSample(
            slotB,
            buffer[i + 1]
        );
    }


    const uint32_t now =
        millis();


    if (
        now - windowStart >=
        AUDIO_REPORT_INTERVAL_MS
    )
    {
        Serial.println(
            "------------------------------------------------"
        );

        printStats(
            "SLOT A",
            slotA
        );

        printStats(
            "SLOT B",
            slotB
        );


        resetStats(
            slotA
        );

        resetStats(
            slotB
        );


        windowStart = now;
    }
}