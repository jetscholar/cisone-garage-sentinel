import argparse
import datetime
import pathlib
import re
import time
import wave

import serial


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "port",
        help="ESP32 serial port, e.g. COM4",
    )

    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
    )

    args = parser.parse_args()


    recordings_dir = pathlib.Path(
        "recordings"
    )

    recordings_dir.mkdir(
        exist_ok=True
    )


    timestamp = datetime.datetime.now().strftime(
        "%Y%m%d-%H%M%S"
    )


    output_path = recordings_dir / (
        f"ring-snapshot-{timestamp}.wav"
    )


    print(
        f"Opening {args.port} at {args.baud} baud..."
    )


    with serial.Serial(
        args.port,
        args.baud,
        timeout=2,
    ) as ser:

        # Opening the ESP32 serial device may reset it.
        # Give the firmware enough time to boot and fill
        # the first 10-second ring.
        print(
            "Waiting 12 seconds for ring buffer to fill..."
        )

        end_time = time.time() + 12


        while time.time() < end_time:
            line = ser.readline()

            if line:
                try:
                    print(
                        line.decode(
                            errors="replace"
                        ).rstrip()
                    )
                except Exception:
                    pass


        print()
        print(
            "Ring should now be full."
        )

        print(
            "Speak a test sequence now."
        )

        print(
            "Snapshot will be requested in 10 seconds..."
        )


        # This 10-second interval becomes the audio
        # expected in the frozen rolling buffer.
        for remaining in range(
            10,
            0,
            -1
        ):
            print(
                f"{remaining}..."
            )

            time.sleep(
                1
            )


        # Discard old textual health output before
        # requesting binary transfer.
        ser.reset_input_buffer()


        print(
            "Requesting snapshot..."
        )


        ser.write(
            b"s\n"
        )

        ser.flush()


        wav_size = None


        while True:
            line = ser.readline()

            if not line:
                continue


            match = re.match(
                rb"WAV_BEGIN\s+(\d+)",
                line.strip(),
            )


            if match:
                wav_size = int(
                    match.group(1)
                )

                break


            try:
                print(
                    line.decode(
                        errors="replace"
                    ).rstrip()
                )
            except Exception:
                pass


        print(
            f"Receiving {wav_size} WAV bytes..."
        )


        remaining = wav_size


        with output_path.open(
            "wb"
        ) as output:

            while remaining > 0:
                chunk = ser.read(
                    min(
                        4096,
                        remaining,
                    )
                )


                if not chunk:
                    raise RuntimeError(
                        "Timed out while receiving WAV data"
                    )


                output.write(
                    chunk
                )

                remaining -= len(
                    chunk
                )


        print(
            f"Saved: {output_path}"
        )


    with wave.open(
        str(output_path),
        "rb",
    ) as wav:

        channels = wav.getnchannels()
        rate = wav.getframerate()
        width = wav.getsampwidth()
        frames = wav.getnframes()

        duration = (
            frames /
            float(rate)
        )


    print()
    print(
        f"Channels    : {channels}"
    )

    print(
        f"Sample rate : {rate} Hz"
    )

    print(
        f"Sample width: {width * 8} bit"
    )

    print(
        f"Frames      : {frames}"
    )

    print(
        f"Duration    : {duration:.2f} seconds"
    )


if __name__ == "__main__":
    main()