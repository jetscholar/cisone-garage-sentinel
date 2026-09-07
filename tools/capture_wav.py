import sys
import time
from datetime import datetime
from pathlib import Path

import serial


BAUD = 115200


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
            f"\rReceiving WAV: "
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
            "Usage: capture_wav.py COM4"
        )

        raise SystemExit(1)


    port = sys.argv[1]


    project_root = (
        Path(__file__)
        .resolve()
        .parent
        .parent
    )


    recordings_dir = (
        project_root /
        "recordings"
    )


    recordings_dir.mkdir(
        parents=True,
        exist_ok=True
    )


    timestamp = (
        datetime.now()
        .strftime(
            "%Y%m%d-%H%M%S"
        )
    )


    wav_path = (
        recordings_dir /
        f"sipeed-i2s-{timestamp}.wav"
    )


    print(
        f"Opening {port} at {BAUD} baud..."
    )


    with serial.Serial(
        port,
        BAUD,
        timeout=30,
    ) as ser:

        # Opening COM may reset the ESP32.
        time.sleep(3)

        ser.reset_input_buffer()


        print(
            "Requesting 10-second recording..."
        )


        ser.write(
            b"RECORD\n"
        )

        ser.flush()


        wav_size = None


        while True:
            line = ser.readline()

            if not line:
                raise TimeoutError(
                    "Timed out waiting "
                    "for WAV_BEGIN."
                )


            text = line.decode(
                "utf-8",
                errors="replace"
            ).strip()


            if text:
                print(text)


            if text.startswith(
                "WAV_BEGIN "
            ):
                parts = (
                    text.split()
                )


                if len(parts) != 2:
                    raise RuntimeError(
                        f"Bad WAV_BEGIN marker: "
                        f"{text}"
                    )


                wav_size = int(
                    parts[1]
                )

                break


        print(
            f"Receiving {wav_size} bytes..."
        )


        wav_data = read_exact(
            ser,
            wav_size
        )


    wav_path.write_bytes(
        wav_data
    )


    print()
    print(
        f"Saved: {wav_path}"
    )

    print(
        f"Size : {len(wav_data)} bytes"
    )


    expected_size = (
        44 +
        48000 *
        10 *
        2
    )


    print(
        f"Expected: {expected_size} bytes"
    )


    if (
        len(wav_data) ==
        expected_size
    ):
        print(
            "WAV size: PASS"
        )
    else:
        print(
            "WAV size: FAIL"
        )


if __name__ == "__main__":
    main()