import argparse
import datetime
import pathlib
import re
import time
import wave

import serial


SNAPSHOT_COUNT = 3
EXPECTED_WAV_BYTES = 960_044
EXPECTED_FRAMES = 480_000
EXPECTED_RATE = 48_000
EXPECTED_WIDTH = 2
EXPECTED_CHANNELS = 1


def print_serial_line(line: bytes) -> str:
    text = line.decode(
        errors="replace"
    ).rstrip()

    if text:
        print(text)

    return text


def wait_for_initial_ring(
    ser: serial.Serial,
    timeout_seconds: int = 30,
):
    print()
    print(
        "Waiting for initial 10-second ring to become full..."
    )

    deadline = (
        time.time()
        + timeout_seconds
    )

    while time.time() < deadline:
        line = ser.readline()

        if not line:
            continue

        text = print_serial_line(
            line
        )

        if (
            text.startswith("Full")
            and "YES" in text
        ):
            print()
            print(
                "Initial ring is full."
            )

            return

    raise RuntimeError(
        "Timed out waiting for the initial ring buffer to fill."
    )


def countdown(
    snapshot_number: int,
):
    print()
    print(
        "========================================"
    )

    print(
        f"Snapshot {snapshot_number} of {SNAPSHOT_COUNT}"
    )

    print(
        "========================================"
    )

    print()
    print(
        "Speak during this countdown."
    )

    print(
        "The snapshot will contain these previous 10 seconds."
    )

    print()

    for remaining in range(
        10,
        0,
        -1,
    ):
        print(
            f"{remaining}..."
        )

        time.sleep(
            1
        )


def request_snapshot(
    ser: serial.Serial,
):
    # Clear old health/report text before issuing
    # the command. This does not affect audio.
    ser.reset_input_buffer()

    ser.write(
        b"s\n"
    )

    ser.flush()


def wait_for_wav_begin(
    ser: serial.Serial,
    timeout_seconds: int = 10,
):
    deadline = (
        time.time()
        + timeout_seconds
    )

    frozen_ring = None
    active_ring = None
    firmware_snapshot_number = None
    wav_size = None

    while time.time() < deadline:
        line = ser.readline()

        if not line:
            continue

        stripped = line.strip()

        match = re.match(
            rb"WAV_BEGIN\s+(\d+)",
            stripped,
        )

        if match:
            wav_size = int(
                match.group(1)
            )

            break

        text = print_serial_line(
            line
        )

        if text.startswith(
            "Snapshot rejected"
        ):
            raise RuntimeError(
                "Firmware rejected the snapshot request."
            )

        if text.startswith(
            "Snapshot       :"
        ):
            firmware_snapshot_number = (
                text.split(
                    ":",
                    1,
                )[1].strip()
            )

        elif text.startswith(
            "Frozen ring    :"
        ):
            frozen_ring = (
                text.split(
                    ":",
                    1,
                )[1].strip()
            )

        elif text.startswith(
            "Active ring    :"
        ):
            active_ring = (
                text.split(
                    ":",
                    1,
                )[1].strip()
            )

    if wav_size is None:
        raise RuntimeError(
            "Timed out waiting for WAV_BEGIN."
        )

    return {
        "wav_size": wav_size,
        "frozen_ring": frozen_ring,
        "active_ring": active_ring,
        "firmware_snapshot_number":
            firmware_snapshot_number,
    }


def receive_wav(
    ser: serial.Serial,
    output_path: pathlib.Path,
    wav_size: int,
):
    print()
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
                    "Timed out while receiving WAV data."
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


def wait_for_concurrency_result(
    ser: serial.Serial,
    timeout_seconds: int = 10,
):
    deadline = (
        time.time()
        + timeout_seconds
    )

    audio_continued = None

    while time.time() < deadline:
        line = ser.readline()

        if not line:
            continue

        text = print_serial_line(
            line
        )

        if text.startswith(
            "Audio continued:"
        ):
            value = (
                text.split(
                    ":",
                    1,
                )[1].strip()
            )

            audio_continued = (
                value == "YES"
            )

            break

    if audio_continued is None:
        raise RuntimeError(
            "Did not receive the snapshot concurrency result."
        )

    return audio_continued


def inspect_wav(
    output_path: pathlib.Path,
):
    file_size = (
        output_path.stat().st_size
    )

    with wave.open(
        str(output_path),
        "rb",
    ) as wav:
        channels = (
            wav.getnchannels()
        )

        rate = (
            wav.getframerate()
        )

        width = (
            wav.getsampwidth()
        )

        frames = (
            wav.getnframes()
        )

        duration = (
            frames
            /
            float(rate)
        )

    valid = (
        file_size
        == EXPECTED_WAV_BYTES
        and channels
        == EXPECTED_CHANNELS
        and rate
        == EXPECTED_RATE
        and width
        == EXPECTED_WIDTH
        and frames
        == EXPECTED_FRAMES
    )

    print()
    print(
        "---- WAV validation ----"
    )

    print(
        f"File size   : {file_size} bytes"
    )

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

    print(
        f"WAV valid   : {'YES' if valid else 'NO'}"
    )

    return {
        "valid": valid,
        "file_size": file_size,
        "channels": channels,
        "rate": rate,
        "width": width,
        "frames": frames,
        "duration": duration,
    }


def validate_alternation(
    results,
):
    frozen = [
        item["frozen_ring"]
        for item in results
    ]

    active = [
        item["active_ring"]
        for item in results
    ]

    alternation_ok = (
        len(results) == 3
        and frozen[0] is not None
        and frozen[1] is not None
        and frozen[2] is not None
        and frozen[0] != frozen[1]
        and frozen[0] == frozen[2]
        and active[0] != frozen[0]
        and active[1] != frozen[1]
        and active[2] != frozen[2]
    )

    return alternation_ok


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


    session_timestamp = (
        datetime.datetime.now().strftime(
            "%Y%m%d-%H%M%S"
        )
    )


    print(
        f"Opening {args.port} at {args.baud} baud..."
    )

    print(
        "Keep this serial connection open for all three snapshots."
    )


    results = []


    with serial.Serial(
        args.port,
        args.baud,
        timeout=5,
    ) as ser:

        # Some ESP32-S3 USB serial configurations reset
        # when the port is opened. Allow startup to begin.
        time.sleep(
            2
        )

        wait_for_initial_ring(
            ser
        )


        for snapshot_number in range(
            1,
            SNAPSHOT_COUNT + 1,
        ):
            countdown(
                snapshot_number
            )


            print()
            print(
                f"Requesting snapshot {snapshot_number}..."
            )


            request_snapshot(
                ser
            )


            metadata = (
                wait_for_wav_begin(
                    ser
                )
            )


            output_path = (
                recordings_dir
                /
                (
                    f"stress-{session_timestamp}"
                    f"-snapshot-{snapshot_number}.wav"
                )
            )


            receive_wav(
                ser,
                output_path,
                metadata[
                    "wav_size"
                ],
            )


            audio_continued = (
                wait_for_concurrency_result(
                    ser
                )
            )


            wav_info = inspect_wav(
                output_path
            )


            result = {
                "number":
                    snapshot_number,

                "path":
                    output_path,

                "frozen_ring":
                    metadata[
                        "frozen_ring"
                    ],

                "active_ring":
                    metadata[
                        "active_ring"
                    ],

                "firmware_snapshot_number":
                    metadata[
                        "firmware_snapshot_number"
                    ],

                "wav_size":
                    metadata[
                        "wav_size"
                    ],

                "audio_continued":
                    audio_continued,

                "wav_valid":
                    wav_info[
                        "valid"
                    ],
            }


            results.append(
                result
            )


            print()
            print(
                "---- Snapshot result ----"
            )

            print(
                f"Frozen ring    : {result['frozen_ring']}"
            )

            print(
                f"Active ring    : {result['active_ring']}"
            )

            print(
                "Audio continued: "
                + (
                    "YES"
                    if audio_continued
                    else "NO"
                )
            )

            print(
                "Snapshot PASS  : "
                + (
                    "YES"
                    if (
                        audio_continued
                        and wav_info[
                            "valid"
                        ]
                    )
                    else "NO"
                )
            )


    alternation_ok = (
        validate_alternation(
            results
        )
    )


    all_wavs_valid = all(
        item[
            "wav_valid"
        ]
        for item in results
    )


    all_audio_continued = all(
        item[
            "audio_continued"
        ]
        for item in results
    )


    overall_pass = (
        len(results)
        == SNAPSHOT_COUNT
        and all_wavs_valid
        and all_audio_continued
        and alternation_ok
    )


    print()
    print(
        "========================================"
    )

    print(
        "0.2.10 SNAPSHOT STRESS RESULT"
    )

    print(
        "========================================"
    )


    for result in results:
        print(
            f"Snapshot {result['number']}: "
            f"{result['frozen_ring']} frozen -> "
            f"{result['active_ring']} active | "
            f"WAV {'PASS' if result['wav_valid'] else 'FAIL'} | "
            f"Audio {'YES' if result['audio_continued'] else 'NO'}"
        )


    print()
    print(
        "Ring alternation : "
        + (
            "PASS"
            if alternation_ok
            else "FAIL"
        )
    )

    print(
        "All WAV files    : "
        + (
            "PASS"
            if all_wavs_valid
            else "FAIL"
        )
    )

    print(
        "Audio continuous : "
        + (
            "PASS"
            if all_audio_continued
            else "FAIL"
        )
    )

    print()
    print(
        "OVERALL          : "
        + (
            "PASS"
            if overall_pass
            else "FAIL"
        )
    )


if __name__ == "__main__":
    main()