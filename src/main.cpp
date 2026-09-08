#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>

#include "driver/i2s.h"
#include "config.h"


// ============================================================
// Audio analysis
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
// Double PSRAM ring buffers
// ============================================================

struct AudioRing
{
    int16_t* data = nullptr;

    size_t capacity = 0;
    size_t writeIndex = 0;

    uint64_t totalWritten = 0;
    uint32_t wraps = 0;

    bool full = false;
};


static AudioRing gRingA;
static AudioRing gRingB;

static AudioRing* gActiveRing =
    nullptr;

static AudioRing* gFrozenRing =
    nullptr;


static volatile bool gSnapshotRequest =
    false;

static volatile bool gSnapshotReady =
    false;


static portMUX_TYPE gRingMux =
    portMUX_INITIALIZER_UNLOCKED;


// ============================================================
// I2S
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
// Microphone settling
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


    Serial.println(
        "Microphone settled."
    );
}


// ============================================================
// Ring allocation
// ============================================================

static bool allocateRing(
    AudioRing& ring,
    const char* name
)
{
    ring.capacity =
        static_cast<size_t>(
            MIC_SAMPLE_RATE
        ) *
        AUDIO_RING_SECONDS;


    const size_t bytes =
        ring.capacity *
        sizeof(int16_t);


    ring.data =
        static_cast<int16_t*>(
            heap_caps_malloc(
                bytes,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            )
        );


    if (ring.data == nullptr)
    {
        Serial.printf(
            "FAIL: %s allocation\n",
            name
        );

        return false;
    }


    memset(
        ring.data,
        0,
        bytes
    );


    ring.writeIndex = 0;
    ring.totalWritten = 0;
    ring.wraps = 0;
    ring.full = false;


    Serial.printf(
        "%s allocation: PASS (%u bytes)\n",
        name,
        static_cast<unsigned>(
            bytes
        )
    );


    return true;
}


static bool setupAudioRings()
{
    Serial.println();
    Serial.println(
        "---- Dual PSRAM audio rings ----"
    );


    Serial.printf(
        "Duration each : %u seconds\n",
        AUDIO_RING_SECONDS
    );


    Serial.printf(
        "Capacity each : %u samples\n",
        static_cast<unsigned>(
            MIC_SAMPLE_RATE *
            AUDIO_RING_SECONDS
        )
    );


    if (
        !allocateRing(
            gRingA,
            "Ring A"
        )
    )
    {
        return false;
    }


    if (
        !allocateRing(
            gRingB,
            "Ring B"
        )
    )
    {
        return false;
    }


    gActiveRing =
        &gRingA;

    gFrozenRing =
        nullptr;


    Serial.printf(
        "Free PSRAM    : %u bytes\n",
        static_cast<unsigned>(
            ESP.getFreePsram()
        )
    );


    return true;
}


// ============================================================
// Analysis snapshot
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
    if (windowSamples == 0)
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


    if (variance < 0.0)
    {
        variance = 0.0;
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

    snapshot.valid = true;

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
// Audio task
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


    AudioRing* activeRing =
        gActiveRing;


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


    while (true)
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


        if (result == ESP_ERR_TIMEOUT)
        {
            ++readTimeouts;
            continue;
        }


        if (result != ESP_OK)
        {
            ++readErrors;

            vTaskDelay(
                pdMS_TO_TICKS(
                    1
                )
            );

            continue;
        }


        if (bytesRead == 0)
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


        for (
            size_t i = 0;
            i + 1 < wordsRead;
            i += 2
        )
        {
            const int32_t sample24 =
                buffer[i] >>
                8;


            int32_t sample16 =
                sample24 >>
                8;


            if (sample16 > 32767)
            {
                sample16 = 32767;
            }
            else if (sample16 < -32768)
            {
                sample16 = -32768;
            }


            activeRing->data[
                ringWriteIndex
            ] =
                static_cast<int16_t>(
                    sample16
                );


            ++ringWriteIndex;
            ++ringTotalWritten;


            if (
                ringWriteIndex >=
                activeRing->capacity
            )
            {
                ringWriteIndex = 0;

                ++ringWraps;

                ringFull = true;
            }


            const double value =
                static_cast<double>(
                    sample24
                );


            sum +=
                value;

            sumSquares +=
                value *
                value;


            if (sample24 < minSample)
            {
                minSample =
                    sample24;
            }


            if (sample24 > maxSample)
            {
                maxSample =
                    sample24;
            }


            ++windowSamples;
            ++totalSamples;
        }


        // Publish current active-ring metadata.
        portENTER_CRITICAL(
            &gRingMux
        );

        activeRing->writeIndex =
            ringWriteIndex;

        activeRing->totalWritten =
            ringTotalWritten;

        activeRing->wraps =
            ringWraps;

        activeRing->full =
            ringFull;


        // Snapshot requests are serviced only at an
        // I2S block boundary.
        //
        // No PCM data is copied here. We simply swap
        // active and frozen ring pointers.
        if (
            gSnapshotRequest &&
            !gSnapshotReady
        )
        {
            AudioRing* oldRing =
                activeRing;


            AudioRing* newRing =
                (
                    oldRing ==
                    &gRingA
                )
                ? &gRingB
                : &gRingA;


            newRing->writeIndex = 0;
            newRing->totalWritten = 0;
            newRing->wraps = 0;
            newRing->full = false;


            gFrozenRing =
                oldRing;

            gActiveRing =
                newRing;


            gSnapshotRequest =
                false;

            gSnapshotReady =
                true;


            activeRing =
                newRing;

            ringWriteIndex = 0;
            ringTotalWritten = 0;
            ringWraps = 0;
            ringFull = false;
        }


        portEXIT_CRITICAL(
            &gRingMux
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


            windowSamples = 0;

            sum = 0.0;

            sumSquares = 0.0;

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

static AudioSnapshot getAudioSnapshot()
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


    return snapshot;
}


static void printAudioReport()
{
    const AudioSnapshot snapshot =
        getAudioSnapshot();


    if (!snapshot.valid)
    {
        Serial.println(
            "[AUDIO] Waiting for first analysis window..."
        );

        return;
    }


    const uint32_t ageMs =
        millis() -
        gLastAudioDataMs;


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
// Active ring report
// ============================================================

static void printRingReport()
{
    size_t capacity;
    size_t writeIndex;

    uint64_t totalWritten;

    uint32_t wraps;

    bool full;


    portENTER_CRITICAL(
        &gRingMux
    );


    AudioRing* active =
        gActiveRing;


    capacity =
        active->capacity;

    writeIndex =
        active->writeIndex;

    totalWritten =
        active->totalWritten;

    wraps =
        active->wraps;

    full =
        active->full;


    portEXIT_CRITICAL(
        &gRingMux
    );


    const uint64_t retained =
        totalWritten >= capacity
        ? capacity
        : totalWritten;


    const double seconds =
        static_cast<double>(
            retained
        ) /
        MIC_SAMPLE_RATE;


    Serial.println();
    Serial.println(
        "---- Active audio ring ----"
    );


    Serial.printf(
        "Capacity      : %u samples\n",
        static_cast<unsigned>(
            capacity
        )
    );


    Serial.printf(
        "Retained      : %llu samples\n",
        static_cast<unsigned long long>(
            retained
        )
    );


    Serial.printf(
        "Retained time : %.2f seconds\n",
        seconds
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
// Memory
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
// Snapshot request
// ============================================================

static void requestSnapshot()
{
    bool allowed =
        false;


    portENTER_CRITICAL(
        &gRingMux
    );


    if (
        !gSnapshotRequest &&
        !gSnapshotReady &&
        gActiveRing != nullptr &&
        gActiveRing->full
    )
    {
        gSnapshotRequest =
            true;

        allowed =
            true;
    }


    portEXIT_CRITICAL(
        &gRingMux
    );


    if (allowed)
    {
        Serial.println();
        Serial.println(
            "Snapshot requested."
        );
    }
    else
    {
        Serial.println();
        Serial.println(
            "Snapshot rejected: active ring is not full or another snapshot is pending."
        );
    }
}


// ============================================================
// WAV helpers
// ============================================================

static void writeLe16(
    uint8_t* target,
    uint16_t value
)
{
    target[0] =
        static_cast<uint8_t>(
            value &
            0xFF
        );

    target[1] =
        static_cast<uint8_t>(
            (
                value >>
                8
            ) &
            0xFF
        );
}


static void writeLe32(
    uint8_t* target,
    uint32_t value
)
{
    target[0] =
        static_cast<uint8_t>(
            value &
            0xFF
        );

    target[1] =
        static_cast<uint8_t>(
            (
                value >>
                8
            ) &
            0xFF
        );

    target[2] =
        static_cast<uint8_t>(
            (
                value >>
                16
            ) &
            0xFF
        );

    target[3] =
        static_cast<uint8_t>(
            (
                value >>
                24
            ) &
            0xFF
        );
}


static void buildWavHeader(
    uint8_t* header,
    uint32_t dataBytes
)
{
    memset(
        header,
        0,
        44
    );


    memcpy(
        header + 0,
        "RIFF",
        4
    );


    writeLe32(
        header + 4,
        36 +
        dataBytes
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


    writeLe32(
        header + 16,
        16
    );


    writeLe16(
        header + 20,
        1
    );


    writeLe16(
        header + 22,
        1
    );


    writeLe32(
        header + 24,
        MIC_SAMPLE_RATE
    );


    writeLe32(
        header + 28,
        MIC_SAMPLE_RATE *
        2
    );


    writeLe16(
        header + 32,
        2
    );


    writeLe16(
        header + 34,
        16
    );


    memcpy(
        header + 36,
        "data",
        4
    );


    writeLe32(
        header + 40,
        dataBytes
    );
}


// ============================================================
// Binary serial writer
// ============================================================

static void writePcmToSerial(
    const int16_t* samples,
    size_t sampleCount
)
{
    const uint8_t* data =
        reinterpret_cast<
            const uint8_t*
        >(
            samples
        );


    size_t remaining =
        sampleCount *
        sizeof(int16_t);


    while (remaining > 0)
    {
        const size_t chunk =
            remaining > 4096
            ? 4096
            : remaining;


        const size_t written =
            Serial.write(
                data,
                chunk
            );


        if (written == 0)
        {
            vTaskDelay(
                pdMS_TO_TICKS(
                    1
                )
            );

            continue;
        }


        data +=
            written;

        remaining -=
            written;
    }
}


// ============================================================
// Frozen ring WAV transfer
// ============================================================

static void transmitFrozenWav()
{
    AudioRing* frozen =
        nullptr;


    size_t capacity =
        0;

    size_t writeIndex =
        0;

    uint64_t totalWritten =
        0;

    bool full =
        false;


    portENTER_CRITICAL(
        &gRingMux
    );


    if (
        gSnapshotReady &&
        gFrozenRing != nullptr
    )
    {
        frozen =
            gFrozenRing;

        capacity =
            frozen->capacity;

        writeIndex =
            frozen->writeIndex;

        totalWritten =
            frozen->totalWritten;

        full =
            frozen->full;
    }


    portEXIT_CRITICAL(
        &gRingMux
    );


    if (frozen == nullptr)
    {
        return;
    }


    const size_t validSamples =
        full
        ? capacity
        : static_cast<size_t>(
            totalWritten
        );


    const uint32_t dataBytes =
        static_cast<uint32_t>(
            validSamples *
            sizeof(int16_t)
        );


    const uint32_t wavBytes =
        44 +
        dataBytes;


    const AudioSnapshot before =
        getAudioSnapshot();


    uint8_t header[
        44
    ];


    buildWavHeader(
        header,
        dataBytes
    );


    Serial.println();
    Serial.println(
        "Snapshot ready."
    );


    Serial.printf(
        "Frozen samples : %u\n",
        static_cast<unsigned>(
            validSamples
        )
    );


    Serial.printf(
        "Frozen seconds : %.2f\n",
        static_cast<double>(
            validSamples
        ) /
        MIC_SAMPLE_RATE
    );


    Serial.printf(
        "Oldest index   : %u\n",
        static_cast<unsigned>(
            full
                ? writeIndex
                : 0
        )
    );


    // capture_wav.py looks for this line,
    // then reads exactly wavBytes binary bytes.
    Serial.printf(
        "WAV_BEGIN %u\n",
        static_cast<unsigned>(
            wavBytes
        )
    );


    Serial.flush();


    Serial.write(
        header,
        sizeof(header)
    );


    if (full)
    {
        // In a full circular buffer, writeIndex is
        // the next write location and therefore also
        // the oldest retained sample.

        const size_t firstSegment =
            capacity -
            writeIndex;


        writePcmToSerial(
            frozen->data +
            writeIndex,
            firstSegment
        );


        if (writeIndex > 0)
        {
            writePcmToSerial(
                frozen->data,
                writeIndex
            );
        }
    }
    else
    {
        writePcmToSerial(
            frozen->data,
            validSamples
        );
    }


    Serial.flush();


    Serial.println();
    Serial.println(
        "WAV_END"
    );


    // Release the frozen ring so that it can become
    // the active buffer at the next snapshot.
    portENTER_CRITICAL(
        &gRingMux
    );

    gFrozenRing =
        nullptr;

    gSnapshotReady =
        false;

    portEXIT_CRITICAL(
        &gRingMux
    );


    delay(
        100
    );


    const AudioSnapshot after =
        getAudioSnapshot();


    Serial.println();
    Serial.println(
        "---- Snapshot concurrency check ----"
    );


    if (
        before.valid &&
        after.valid
    )
    {
        Serial.printf(
            "Samples before : %llu\n",
            static_cast<unsigned long long>(
                before.totalSamples
            )
        );


        Serial.printf(
            "Samples after  : %llu\n",
            static_cast<unsigned long long>(
                after.totalSamples
            )
        );


        Serial.printf(
            "Samples gained : %llu\n",
            static_cast<unsigned long long>(
                after.totalSamples -
                before.totalSamples
            )
        );


        Serial.printf(
            "Read errors    : %u\n",
            after.readErrors
        );


        Serial.printf(
            "Timeouts       : %u\n",
            after.readTimeouts
        );


        Serial.printf(
            "Zero reads     : %u\n",
            after.zeroReads
        );


        const uint32_t ageMs =
            millis() -
            gLastAudioDataMs;


        Serial.printf(
            "Last data age  : %u ms\n",
            ageMs
        );


        Serial.printf(
            "Audio continued: %s\n",
            (
                after.totalSamples >
                before.totalSamples &&
                ageMs <=
                AUDIO_HEALTH_TIMEOUT_MS
            )
            ? "YES"
            : "NO"
        );
    }
}


// ============================================================
// Serial commands
// ============================================================

static void pollSerialCommands()
{
    while (Serial.available() > 0)
    {
        const char command =
            static_cast<char>(
                Serial.read()
            );


        if (
            command == 's' ||
            command == 'S'
        )
        {
            requestSnapshot();
        }
    }
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
        "Phase 3 - rolling audio snapshot proof"
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


    if (!setupMicrophone())
    {
        Serial.println(
            "I2S INITIALIZATION FAILED"
        );

        while (true)
        {
            delay(
                1000
            );
        }
    }


    settleMicrophone();


    if (!setupAudioRings())
    {
        Serial.println(
            "AUDIO RING INITIALIZATION FAILED"
        );

        while (true)
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


    if (taskResult != pdPASS)
    {
        Serial.println(
            "FAIL: could not create audio task"
        );

        while (true)
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
        "Dual rolling audio buffers running."
    );

    Serial.println(
        "Send 's' to snapshot the previous 10 seconds."
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


    pollSerialCommands();


    if (gSnapshotReady)
    {
        transmitFrozenWav();
    }


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