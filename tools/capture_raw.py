import json
import sys
import time
from datetime import datetime
from pathlib import Path

import serial


BAUD = 115200

SAMPLE_RATE = 16000
CHANNEL_SLOTS = 2
WORD_BITS = 32
CAPTURE_SECONDS = 2


def read_exact(ser, count):
    data = bytearray()

    chunk_size = 4096

    while len(data) < count:
        remaining = count - len(data)

        chunk = ser.read(
            min(chunk_size, remaining)
        )

        if not chunk:
            raise TimeoutError(
                f"Serial timeout after "
                f"{len(data)}/{count} bytes"
            )

        data.extend(chunk)

        percentage = (
            len(data) *
            100.0 /
            count
        )

        print(
            f"\rReceiving raw data: "
            f"{percentage:5.1f}%  "
            f"({len(data)}/{count} bytes)",
            end="",
            flush=True,
        )

    print()

    return bytes(data)


def main():
    if len(sys.argv) < 2:
        print(
            "Usage: capture_raw.py COM4"
        )

        raise SystemExit(1)


    port = sys.argv[1]


    project_root = (
        Path(__file__).resolve()
        .parent
        .parent
    )


    capture_dir = (
        project_root /
        "recordings" /
        "raw"
    )


    capture_dir.mkdir(
        parents=True,
        exist_ok=True
    )


    timestamp = (
        datetime.now()
        .strftime(
            "%Y%m%d-%H%M%S"
        )
    )


    raw_path = (
        capture_dir /
        f"inmp441-raw-{timestamp}.bin"
    )


    metadata_path = (
        capture_dir /
        f"inmp441-raw-{timestamp}.json"
    )


    print(
        f"Opening {port} at {BAUD} baud..."
    )


    with serial.Serial(
        port,
        BAUD,
        timeout=30,
    ) as ser:

        # Opening the port may reset the ESP32.
        time.sleep(3)

        ser.reset_input_buffer()


        print(
            "Requesting raw capture..."
        )


        ser.write(
            b"CAPTURE\n"
        )

        ser.flush()


        raw_size = None


        while True:
            line = ser.readline()

            if not line:
                raise TimeoutError(
                    "Timed out waiting "
                    "for RAW_BEGIN."
                )


            text = line.decode(
                "utf-8",
                errors="replace"
            ).strip()


            if text:
                print(text)


            if text.startswith(
                "RAW_BEGIN "
            ):
                parts = (
                    text.split()
                )


                if len(parts) != 2:
                    raise RuntimeError(
                        f"Bad RAW_BEGIN marker: "
                        f"{text}"
                    )


                raw_size = int(
                    parts[1]
                )

                break


        print(
            f"Receiving {raw_size} bytes..."
        )


        raw_data = read_exact(
            ser,
            raw_size
        )


    raw_path.write_bytes(
        raw_data
    )


    metadata = {
        "project":
            "cisone-garage-sentinel",

        "firmware":
            "0.2.4",

        "format":
            "raw ESP32 I2S driver output",

        "endianness":
            "little-endian",

        "word_type":
            "signed int32",

        "word_bits":
            WORD_BITS,

        "sample_rate_hz":
            SAMPLE_RATE,

        "slot_mode":
            "RIGHT_LEFT",

        "slots_per_frame":
            CHANNEL_SLOTS,

        "capture_seconds":
            CAPTURE_SECONDS,

        "lr_hardware_strap":
            "GND",

        "processing":
            "none",

        "shift":
            0,

        "normalisation":
            False,

        "dc_removal":
            False,

        "raw_bytes":
            len(raw_data),
    }


    metadata_path.write_text(
        json.dumps(
            metadata,
            indent=2
        ),
        encoding="utf-8",
    )


    print()
    print(
        f"Saved raw data: {raw_path}"
    )

    print(
        f"Saved metadata: {metadata_path}"
    )

    print(
        f"Raw size      : {len(raw_data)} bytes"
    )


    expected_size = (
        SAMPLE_RATE *
        CHANNEL_SLOTS *
        CAPTURE_SECONDS *
        4
    )


    print(
        f"Expected size : {expected_size} bytes"
    )


    if (
        len(raw_data) ==
        expected_size
    ):
        print(
            "Capture size  : PASS"
        )
    else:
        print(
            "Capture size  : FAIL"
        )


if __name__ == "__main__":
    main()