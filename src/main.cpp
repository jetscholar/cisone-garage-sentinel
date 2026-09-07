#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>

#include "driver/i2s.h"
#include "config.h"


// ============================================================
// WAV helpers
// ============================================================

static void writeLE16(
    uint8_t* dst,
    uint16_t value
)
{
    dst[0] = value & 0xFF;
    dst[1] = (value >> 8) & 0xFF;
}


static void writeLE32(
    uint8_t* dst,
    uint32_t value
)
{
    dst[0] = value & 0xFF;
    dst[1] = (value >> 8) & 0xFF;
    dst[2] = (value >> 16) & 0xFF;
    dst[3] = (value >> 24) & 0xFF;
}


static void buildWavHeader(
    uint8_t* header,
    uint32_t sampleRate,
    uint32_t sampleCount
)
{
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;

    const uint32_t dataBytes =
        sampleCount *
        channels *
        (bitsPerSample / 8);

    const uint32_t byteRate =
        sampleRate *
        channels *
        (bitsPerSample / 8);

    const uint16_t blockAlign =
        channels *
        (bitsPerSample / 8);


    memcpy(
        header + 0,
        "RIFF",
        4
    );

    writeLE32(
        header + 4,
        36 + dataBytes
    );

    memcpy(
        header + 8,
        "WAVE",
        4
    );

    memcpy(
        header + 12,
        "fmt ",
        4
    );

    writeLE32(
        header + 16,
        16
    );

    writeLE16(
        header + 20,
        1
    );

    writeLE16(
        header + 22,
        channels
    );

    writeLE32(
        header + 24,
        sampleRate
    );

    writeLE32(
        header + 28,
        byteRate
    );

    writeLE16(
        header + 32,
        blockAlign
    );

    writeLE16(
        header + 34,
        bitsPerSample
    );

    memcpy(
        header + 36,
        "data",
        4
    );

    writeLE32(
        header + 40,
        dataBytes
    );
}


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
        // Diagnostic 0.2.5 established that
        // buffer[0], buffer[2], ... is SLOT A,
        // which is our active microphone slot.
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
        "Sample      : raw >> 8 = signed 24-bit"
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
// Discard startup transient
// ============================================================

static void settleMicrophone()
{
    int32_t buffer[MIC_READ_WORDS];

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
        size_t bytesRead = 0;

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
}


// ============================================================
// Capture active SLOT A
// ============================================================

static bool captureAudio(
    int32_t* audio24,
    size_t sampleCount
)
{
    int32_t buffer[
        MIC_READ_WORDS
    ];


    size_t captured =
        0;


    while (
        captured <
        sampleCount
    )
    {
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
                "FAIL: i2s_read(): %s\n",
                esp_err_to_name(result)
            );

            return false;
        }


        const size_t wordsRead =
            bytesRead /
            sizeof(int32_t);


        // Stereo interleaved:
        //
        // buffer[0] = SLOT A
        // buffer[1] = SLOT B
        // buffer[2] = SLOT A
        // buffer[3] = SLOT B
        //
        // SLOT A was confirmed as active.
        for (
            size_t i = 0;
            i + 1 < wordsRead &&
            captured < sampleCount;
            i += 2
        )
        {
            const int32_t raw =
                buffer[i];

            // Diagnostic showed low 8 bits
            // are always zero.
            //
            // Convert the aligned 32-bit
            // I2S word to signed 24-bit PCM.
            audio24[captured] =
                raw >> 8;

            ++captured;
        }
    }


    return
        captured ==
        sampleCount;
}


// ============================================================
// Analyse and convert
// ============================================================

static void convertToPcm16(
    const int32_t* audio24,
    int16_t* pcm16,
    size_t sampleCount
)
{
    int64_t sum =
        0;


    for (
        size_t i = 0;
        i < sampleCount;
        ++i
    )
    {
        sum +=
            audio24[i];
    }


    const double mean =
        static_cast<double>(sum) /
        static_cast<double>(
            sampleCount
        );


    long double sumSquares =
        0.0;


    int32_t minCentered =
        INT32_MAX;

    int32_t maxCentered =
        INT32_MIN;

    int64_t peak =
        0;


    for (
        size_t i = 0;
        i < sampleCount;
        ++i
    )
    {
        const int32_t centered =
            static_cast<int32_t>(
                static_cast<double>(
                    audio24[i]
                ) - mean
            );


        if (
            centered <
            minCentered
        )
        {
            minCentered =
                centered;
        }


        if (
            centered >
            maxCentered
        )
        {
            maxCentered =
                centered;
        }


        int64_t magnitude =
            centered;

        if (
            magnitude < 0
        )
        {
            magnitude =
                -magnitude;
        }


        if (
            magnitude > peak
        )
        {
            peak =
                magnitude;
        }


        sumSquares +=
            static_cast<long double>(
                centered
            ) *
            static_cast<long double>(
                centered
            );


        // 24-bit signed PCM -> 16-bit PCM.
        //
        // No automatic normalisation.
        int32_t sample16 =
            centered >> 8;


        if (
            sample16 > 32767
        )
        {
            sample16 =
                32767;
        }
        else if (
            sample16 < -32768
        )
        {
            sample16 =
                -32768;
        }


        pcm16[i] =
            static_cast<int16_t>(
                sample16
            );
    }


    const double rms =
        sqrt(
            static_cast<double>(
                sumSquares /
                static_cast<long double>(
                    sampleCount
                )
            )
        );


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
        peak > 0
        ? 20.0 *
          log10(
              static_cast<double>(
                  peak
              ) /
              fullScale24
          )
        : -120.0;


    Serial.println();
    Serial.println(
        "---- Recording analysis ----"
    );

    Serial.printf(
        "DC mean       : %.1f\n",
        mean
    );

    Serial.printf(
        "Centered range: %ld .. %ld\n",
        static_cast<long>(
            minCentered
        ),
        static_cast<long>(
            maxCentered
        )
    );

    Serial.printf(
        "24-bit RMS    : %.1f\n",
        rms
    );

    Serial.printf(
        "24-bit peak   : %lld\n",
        static_cast<long long>(
            peak
        )
    );

    Serial.printf(
        "RMS level     : %.1f dBFS\n",
        rmsDbfs
    );

    Serial.printf(
        "Peak level    : %.1f dBFS\n",
        peakDbfs
    );

    Serial.println(
        "Gain          : 1.0"
    );

    Serial.println(
        "Normalisation : OFF"
    );
}


// ============================================================
// Record and send WAV
// ============================================================

static void performRecording()
{
    const size_t sampleCount =
        static_cast<size_t>(
            MIC_SAMPLE_RATE
        ) *
        RECORD_SECONDS;


    const size_t audio24Bytes =
        sampleCount *
        sizeof(int32_t);


    const size_t pcm16Bytes =
        sampleCount *
        sizeof(int16_t);


    Serial.println();
    Serial.println(
        "========================================"
    );

    Serial.printf(
        "Recording %u seconds at %u Hz...\n",
        RECORD_SECONDS,
        MIC_SAMPLE_RATE
    );

    Serial.printf(
        "Samples       : %u\n",
        static_cast<unsigned>(
            sampleCount
        )
    );

    Serial.printf(
        "24-bit buffer : %.2f MiB\n",
        static_cast<double>(
            audio24Bytes
        ) /
        (1024.0 * 1024.0)
    );

    Serial.printf(
        "PCM16 buffer  : %.2f KiB\n",
        static_cast<double>(
            pcm16Bytes
        ) /
        1024.0
    );


    int32_t* audio24 =
        static_cast<int32_t*>(
            heap_caps_malloc(
                audio24Bytes,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            )
        );


    if (
        audio24 == nullptr
    )
    {
        Serial.println(
            "FAIL: audio24 PSRAM allocation"
        );

        return;
    }


    int16_t* pcm16 =
        static_cast<int16_t*>(
            heap_caps_malloc(
                pcm16Bytes,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            )
        );


    if (
        pcm16 == nullptr
    )
    {
        Serial.println(
            "FAIL: pcm16 PSRAM allocation"
        );

        heap_caps_free(
            audio24
        );

        return;
    }


    settleMicrophone();


    Serial.println(
        "CAPTURE START"
    );


    const uint32_t captureStart =
        millis();


    const bool success =
        captureAudio(
            audio24,
            sampleCount
        );


    const uint32_t captureElapsed =
        millis() -
        captureStart;


    Serial.printf(
        "CAPTURE END: %u ms\n",
        captureElapsed
    );


    if (
        !success
    )
    {
        Serial.println(
            "FAIL: capture incomplete"
        );

        heap_caps_free(
            pcm16
        );

        heap_caps_free(
            audio24
        );

        return;
    }


    convertToPcm16(
        audio24,
        pcm16,
        sampleCount
    );


    uint8_t wavHeader[44];


    buildWavHeader(
        wavHeader,
        MIC_SAMPLE_RATE,
        sampleCount
    );


    const size_t wavBytes =
        sizeof(wavHeader) +
        pcm16Bytes;


    Serial.println();
    Serial.printf(
        "WAV bytes     : %u\n",
        static_cast<unsigned>(
            wavBytes
        )
    );

    Serial.println(
        "Sending WAV over serial..."
    );


    // Python waits for this exact marker.
    Serial.printf(
        "WAV_BEGIN %u\n",
        static_cast<unsigned>(
            wavBytes
        )
    );

    Serial.flush();


    Serial.write(
        wavHeader,
        sizeof(wavHeader)
    );


    Serial.write(
        reinterpret_cast<
            const uint8_t*
        >(pcm16),
        pcm16Bytes
    );


    Serial.flush();


    heap_caps_free(
        pcm16
    );

    heap_caps_free(
        audio24
    );


    i2s_zero_dma_buffer(
        MIC_I2S_PORT
    );


    Serial.println();
    Serial.println(
        "WAV_END"
    );

    Serial.println(
        "Recording complete."
    );

    Serial.println(
        "Send RECORD to repeat."
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

    Serial.setTimeout(
        100
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
        "Phase 2.5 - Sipeed microphone WAV proof"
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


    Serial.println();
    Serial.println(
        "Sipeed microphone L/R: GND"
    );

    Serial.println(
        "Ready."
    );

    Serial.println(
        "Send RECORD for a 10-second WAV."
    );
}


// ============================================================
// Loop
// ============================================================

void loop()
{
    if (
        Serial.available()
    )
    {
        String command =
            Serial.readStringUntil(
                '\n'
            );


        command.trim();


        if (
            command.equalsIgnoreCase(
                "RECORD"
            )
        )
        {
            performRecording();
        }
    }


    delay(
        10
    );
}