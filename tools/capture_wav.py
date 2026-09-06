import sys
import time
from datetime import datetime
from pathlib import Path

import serial


BAUD = 115200


def read_exact(ser, count):
    data = bytearray()

    while len(data) < count:
        chunk = ser.read(count - len(data))

        if not chunk:
            raise TimeoutError(
                f"Serial timeout after "
                f"{len(data)}/{count} bytes"
            )

        data.extend(chunk)

        percent = (
            len(data) * 100.0 / count
        )

        print(
            f"\rReceiving WAV: "
            f"{percent:5.1f}%  "
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


    recordings_dir = (
        Path(__file__).resolve().parent.parent
        / "recordings"
    )

    recordings_dir.mkdir(
        exist_ok=True
    )


    timestamp = datetime.now().strftime(
        "%Y%m%d-%H%M%S"
    )

    output_path = (
        recordings_dir
        / f"inmp441-{timestamp}.wav"
    )


    print(
        f"Opening {port} at {BAUD} baud..."
    )


    with serial.Serial(
        port,
        BAUD,
        timeout=20,
    ) as ser:

        # Opening the serial port may reset the ESP32.
        time.sleep(3)

        ser.reset_input_buffer()

        print(
            "Requesting 10-second recording..."
        )

        ser.write(
            b"RECORD\n"
        )

        ser.flush()


        while True:
            line = ser.readline()

            if not line:
                raise TimeoutError(
                    "Timed out waiting for ESP32."
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
                parts = text.split()

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
            f"Receiving {wav_size} WAV bytes..."
        )


        wav_data = read_exact(
            ser,
            wav_size
        )


        output_path.write_bytes(
            wav_data
        )


    print()
    print(
        f"Saved: {output_path}"
    )

    print(
        f"Size : {output_path.stat().st_size} bytes"
    )


if __name__ == "__main__":
    main()