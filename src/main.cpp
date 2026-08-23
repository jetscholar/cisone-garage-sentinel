#include <Arduino.h>
#include <esp_heap_caps.h>

#include "config.h"


// ============================================================
// Helpers
// ============================================================

static float bytesToMiB(size_t bytes)
{
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}


// ============================================================
// PSRAM test
// ============================================================

static bool testPsram()
{
    Serial.println();
    Serial.println("---- PSRAM memory test ----");

    if (!psramFound())
    {
        Serial.println("FAIL: PSRAM not detected.");
        return false;
    }

    Serial.printf(
        "Allocating %u bytes (%.2f MiB) in PSRAM...\n",
        static_cast<unsigned>(PSRAM_TEST_SIZE),
        bytesToMiB(PSRAM_TEST_SIZE)
    );

    uint8_t* buffer = static_cast<uint8_t*>(
        heap_caps_malloc(
            PSRAM_TEST_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        )
    );

    if (buffer == nullptr)
    {
        Serial.println("FAIL: PSRAM allocation failed.");
        return false;
    }

    Serial.println("Writing test pattern...");

    for (size_t i = 0; i < PSRAM_TEST_SIZE; ++i)
    {
        buffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    Serial.println("Verifying test pattern...");

    for (size_t i = 0; i < PSRAM_TEST_SIZE; ++i)
    {
        const uint8_t expected = static_cast<uint8_t>(i & 0xFF);

        if (buffer[i] != expected)
        {
            Serial.printf(
                "FAIL: PSRAM mismatch at byte %u: expected %u, got %u\n",
                static_cast<unsigned>(i),
                static_cast<unsigned>(expected),
                static_cast<unsigned>(buffer[i])
            );

            heap_caps_free(buffer);
            return false;
        }
    }

    heap_caps_free(buffer);

    Serial.println("PASS: PSRAM write/read verification succeeded.");
    return true;
}


// ============================================================
// Hardware report
// ============================================================

static void printHardwareReport()
{
    Serial.println();
    Serial.println("================================================");
    Serial.println(PROJECT_NAME);
    Serial.printf("Firmware: %s\n", FW_VERSION);
    Serial.println("================================================");

    Serial.println();
    Serial.println("---- ESP32 ----");

    Serial.printf("Chip model       : %s\n", ESP.getChipModel());
    Serial.printf("Chip revision    : %u\n", ESP.getChipRevision());
    Serial.printf("CPU cores        : %u\n", ESP.getChipCores());
    Serial.printf("CPU frequency    : %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Arduino SDK      : %s\n", ESP.getSdkVersion());

    Serial.println();
    Serial.println("---- Flash ----");

    const size_t flashSize = ESP.getFlashChipSize();

    Serial.printf(
        "Flash size       : %u bytes (%.2f MiB)\n",
        static_cast<unsigned>(flashSize),
        bytesToMiB(flashSize)
    );

    Serial.printf(
        "Flash speed      : %u MHz\n",
        static_cast<unsigned>(ESP.getFlashChipSpeed() / 1000000)
    );

    Serial.println();
    Serial.println("---- Internal RAM ----");

    Serial.printf(
        "Heap size        : %u bytes\n",
        static_cast<unsigned>(ESP.getHeapSize())
    );

    Serial.printf(
        "Free heap        : %u bytes\n",
        static_cast<unsigned>(ESP.getFreeHeap())
    );

    Serial.printf(
        "Minimum free heap: %u bytes\n",
        static_cast<unsigned>(ESP.getMinFreeHeap())
    );

    Serial.println();
    Serial.println("---- PSRAM ----");

    const bool psramPresent = psramFound();

    Serial.printf(
        "PSRAM detected   : %s\n",
        psramPresent ? "YES" : "NO"
    );

    if (psramPresent)
    {
        Serial.printf(
            "PSRAM size       : %u bytes (%.2f MiB)\n",
            static_cast<unsigned>(ESP.getPsramSize()),
            bytesToMiB(ESP.getPsramSize())
        );

        Serial.printf(
            "Free PSRAM       : %u bytes (%.2f MiB)\n",
            static_cast<unsigned>(ESP.getFreePsram()),
            bytesToMiB(ESP.getFreePsram())
        );
    }

    Serial.println();
}


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(SERIAL_BAUD);

    delay(2000);

    printHardwareReport();

    const bool psramOk = testPsram();

    Serial.println();
    Serial.println("================================================");

    if (psramOk)
    {
        Serial.println("PHASE 1 HARDWARE BASELINE: PASS");
    }
    else
    {
        Serial.println("PHASE 1 HARDWARE BASELINE: FAIL");
    }

    Serial.println("================================================");
}


// ============================================================
// Loop
// ============================================================

void loop()
{
    static uint32_t lastHeartbeat = 0;
    static uint32_t heartbeatCount = 0;

    const uint32_t now = millis();

    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS)
    {
        lastHeartbeat = now;
        ++heartbeatCount;

        Serial.printf(
            "[heartbeat %lu] uptime=%lu s free_heap=%u free_psram=%u\n",
            static_cast<unsigned long>(heartbeatCount),
            static_cast<unsigned long>(now / 1000),
            static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getFreePsram())
        );
    }

    delay(10);
}