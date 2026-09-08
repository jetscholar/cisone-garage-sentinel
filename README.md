# cisone-garage-sentinel
<img src="docs/cisone_garage_sentinel_logo.png" alt="Garage roofline and security shield logo with a camera lens, sound waves, and the text cisone garage sentinel in teal on a light gray background" width="50%">

## Version 0.2.6 - September 7 2026

`cisone-garage-sentinel` is a dedicated garage security node for the **cisOne** system, built around an **ESP32-S3-WROOM N16R8 camera module** and a **Sipeed I2S_Mic** digital MEMS microphone.

The project is being rebuilt from the ground up after an earlier ESP32-CAM prototype proved useful for experimentation but never produced reliably audible microphone recordings. Development proceeds in tightly controlled phases, with each subsystem validated independently before integration.

**Current state:** ESP32-S3 hardware baseline and microphone proof are complete. Development is now entering **Phase 3 — Stable Audio Subsystem**.

## Project Goals

The completed device is intended to provide:

- reliable garage video monitoring;
- continuous I2S audio acquisition;
- clearly recognisable recorded audio;
- acoustic event detection;
- still-image and video capture;
- low-light operation;
- concurrent camera and microphone operation;
- local device health monitoring;
- secure integration with cisOne;
- future motion and person detection;
- future event-linked audio and image recording.

The project will not progress to higher-level security functions until the lower-level hardware, audio and camera milestones have been proven.

## Hardware

Current target hardware:

- ESP32-S3-WROOM N16R8
  - 16 MB flash
  - 8 MB PSRAM
- Camera module
  - exact sensor and pin mapping to be verified during Phase 4
- Sipeed I2S_Mic
  - MSM261S4030H0 digital MEMS microphone
  - 3.3 V supply
  - 48 kHz test sample rate
  - 24-bit microphone data carried in 32-bit I2S slots
  - current ESP32-S3 connections:
    - `SD` → GPIO21
    - `SCK` → GPIO42
    - `WS` → GPIO41
    - `L/R` → GND

Previous microphone hardware tested during Phase 2:

- two low-cost INMP441 modules produced structured I2S data but failed the intelligible-audio acceptance test;
- one ICS-43434 breakout produced no usable I2S data and was rejected;
- the Sipeed I2S_Mic produced clearly intelligible speech and is now the project reference microphone.

Additional hardware should be added only after the camera and microphone subsystems are independently validated.

## Development Environment

- Visual Studio Code
- PlatformIO
- Arduino framework
- C/C++
- PowerShell 7
- Git
- cisOne backend integration in later phases

Repository location:

```text
C:\Users\mike_\Documents\coding\smartHome\cisone-garage-sentinel
```

## Project Structure

```text
cisone-garage-sentinel/
├── platformio.ini
├── .gitignore
├── README.md
│
├── include/
│   ├── config.h
│   └── env.h
│
├── src/
│   └── main.cpp
│
├── tools/
│   └── capture_wav.py
│
├── recordings/          # local test captures; ignored by Git
├── lib/
├── test/
└── docs/
```

### Configuration Files

`config.h`

Contains non-secret project configuration, feature settings, hardware-independent constants, firmware version information, and compile-time defaults.

`env.h`

Contains deployment-specific and private configuration such as:

- Wi-Fi credentials;
- static IP configuration;
- cisOne server address;
- API credentials;
- OTA credentials;
- device-specific network settings.

`env.h` must not be committed to Git.

A sanitised template may later be added as:

```text
include/env.example.h
```

Local recordings should also remain outside Git history:

```text
recordings/
```

## Development Principles

### 1. Validate one subsystem at a time

The previous prototype combined camera, microphone, HTTP, cisOne uploads, triggering, streaming and OTA before the microphone had been fully proven.

This project avoids that approach.

Each phase has a defined acceptance milestone. Development proceeds only when that milestone passes.

### 2. Audible microphone recording is mandatory

Changing sample values or detecting noise is not sufficient proof that a microphone is working correctly.

The microphone milestone is:

> Recorded PCM/WAV audio must contain clearly recognisable speech, claps and other test sounds when played back on the development computer.

This milestone was achieved with the Sipeed I2S_Mic at firmware version `0.2.6`.

### 3. Camera and microphone must operate concurrently

Live video must not disable:

- microphone acquisition;
- acoustic event detection;
- device health reporting;
- OTA;
- cisOne communication.

The final firmware architecture will separate time-critical audio acquisition from camera streaming and network activity.

### 4. Security functions must fail predictably

The device must expose meaningful health state for:

- camera;
- microphone;
- Wi-Fi;
- PSRAM;
- cisOne connectivity;
- event upload status.

A powered device must not automatically be considered a healthy security node.

# Phase Milestones

## Phase 1 — ESP32-S3 Hardware Baseline

**Status: COMPLETE**

### Objectives

- establish the PlatformIO project;
- verify the ESP32-S3 toolchain;
- compile and upload firmware;
- confirm serial communication;
- identify the chip correctly;
- confirm CPU configuration;
- confirm 16 MB flash;
- confirm 8 MB PSRAM;
- perform a PSRAM allocation/write/read test;
- establish a stable heartbeat.

### Acceptance Criteria

```text
PlatformIO build        PASS
Firmware upload         PASS
Serial monitor          PASS
Chip                    ESP32-S3
Flash                   16 MB
PSRAM                   8 MB
PSRAM allocation test   PASS
Stable heartbeat        PASS
```

Phase 1 was completed at version `0.1.1`.

## Phase 2 — I2S Microphone Proof

**Status: COMPLETE**

### Objectives

- connect and test an I2S microphone independently of the camera;
- configure ESP32-S3 I2S;
- verify channel and pin configuration;
- inspect raw sample values;
- determine sample alignment;
- measure minimum, maximum, peak and RMS levels;
- capture PCM audio;
- create a valid WAV recording;
- transfer the recording to the development computer;
- listen to the recording and verify intelligibility.

### Diagnostic History

The original INMP441 modules produced structured I2S data and a clearly isolated active channel, but WAV analysis showed overwhelmingly low-frequency energy and no intelligible speech. Raw stereo-slot capture confirmed that the failure was not explained by serial transfer, WAV construction, simple byte ordering, or left/right selection.

A later ICS-43434 breakout returned effectively zero data on both I2S slots and was rejected as non-functional for this project.

The Sipeed I2S_Mic was then tested using the same ESP32-S3 I2S pins. It produced a strong active slot, responded to acoustic input, and showed a clean 24-bit alignment with the low eight bits of each 32-bit I2S word unused. The working sample extraction is:

```cpp
sample24 = raw >> 8;
```

A 10-second mono WAV recorded at 48 kHz contained clearly intelligible speech when played on the development computer.

### Acceptance Criteria

The recorded audio must contain clearly recognisable:

- spoken words;
- hand claps;
- nearby environmental sounds.

```text
I2S acquisition         PASS
Active channel          PASS
24-bit sample alignment PASS
WAV construction        PASS
Serial WAV transfer     PASS
Intelligible speech     PASS
```

Phase 2 was completed at version `0.2.6` using the Sipeed I2S_Mic.

## Phase 3 — Stable Audio Subsystem

**Status: IN PROGRESS**

### Purpose

Phase 3 converts the successful microphone proof into a reliable background audio service suitable for later camera concurrency. Automatic sound-event detection is deliberately deferred until Phase 6.

### Objectives

- move continuous I2S acquisition into a dedicated FreeRTOS task;
- keep I2S acquisition running independently of foreground work;
- implement bounded I2S read timeouts;
- add read-error, timeout and recovery counters;
- implement controlled microphone/I2S recovery;
- calculate continuous RMS and dBFS level windows;
- establish a circular audio ring buffer in PSRAM;
- retain several seconds of recent PCM audio continuously;
- support manual WAV extraction from the buffered audio without stopping acquisition;
- monitor heap, minimum free heap, PSRAM and task health;
- perform a 30–60 minute endurance test.

### Proposed Architecture

```text
Sipeed I2S_Mic
      │
      ▼
Dedicated I2S RX task
      │
      ├──► RMS / dBFS monitoring
      │
      ├──► error / health counters
      │
      └──► circular PSRAM audio buffer
                    │
                    ▼
              manual test capture
                    │
                    ▼
                   WAV
```

### Acceptance Criteria

- continuous audio acquisition remains stable for at least 30–60 minutes;
- no I2S lockups or watchdog resets occur;
- read failures cannot block the device indefinitely;
- recovery behaviour is observable and controlled;
- RMS/dBFS values respond sensibly to quiet, speech and impulsive sounds;
- the PSRAM ring buffer updates continuously;
- repeated manual WAV extraction remains clearly intelligible;
- no progressive heap or PSRAM loss is observed;
- audio subsystem health can be reported accurately.

Phase 3 does **not** require automatic sound triggering, event classification, camera capture or network upload.

## Phase 4 — Camera Proof

**Status: PENDING**

### Objectives

- identify the exact camera sensor and pin mapping;
- initialise the camera independently;
- capture JPEG still images;
- establish stable image quality;
- verify PSRAM framebuffer operation;
- establish live video streaming;
- assess low-light performance.

### Acceptance Criteria

- repeatable still capture;
- stable live stream;
- no camera initialization failures;
- no unexplained PSRAM or heap degradation.

## Phase 5 — Concurrent Camera and Microphone

**Status: PENDING**

### Objectives

- run continuous microphone acquisition while the camera is active;
- separate critical work using FreeRTOS tasks where appropriate;
- ensure live streaming does not interrupt audio acquisition;
- ensure image capture does not interrupt audio acquisition;
- monitor CPU, heap and PSRAM usage;
- establish safe task priorities and queues.

### Acceptance Criteria

While live video is being streamed:

- microphone sampling continues;
- audio remains intelligible;
- HTTP control remains responsive;
- health reporting continues;
- no watchdog resets occur.

This phase is a major architectural gate. Camera streaming must never recreate the blocking-audio behaviour of the earlier prototype.

## Phase 6 — Security Event Detection

**Status: PENDING**

### Objectives

- establish startup ambient calibration;
- calculate acoustic baseline;
- detect significant sound events;
- add debounce and cooldown logic;
- retain pre-trigger audio using the ring buffer;
- capture post-trigger audio;
- associate sound events with camera images;
- investigate motion detection;
- investigate person detection;
- develop false-positive handling for repetitive motion or sound sources.

### Acceptance Criteria

A detected event produces a coherent local event record containing appropriate metadata and media without disrupting ongoing monitoring.

## Phase 7 — cisOne Integration

**Status: PENDING**

### Objectives

- assign the device identity `esp32cam-garage`;
- implement Wi-Fi and unattended reconnection;
- implement authenticated cisOne communication;
- publish device health;
- upload security events;
- upload image attachments;
- upload audio attachments;
- correlate image and audio under a common event ID;
- integrate with the cisOne garage security zone;
- support OTA update securely.

### Acceptance Criteria

cisOne can determine:

- whether the device is online;
- whether the camera is healthy;
- whether the microphone is healthy;
- when the last security event occurred;
- whether the most recent upload succeeded;
- which image and audio recordings belong to an event.

## Future Phases

Possible later work includes:

- local person detection;
- motion classification;
- event scoring;
- live audio listening;
- two-way audio;
- infrared illumination;
- secure browser controls;
- event retention rules;
- local buffering during cisOne outages;
- watchdog and self-recovery mechanisms.

These are deliberately outside the initial hardware validation phases.

# Versioning

The project uses semantic-style firmware versions tied to tested development milestones.

Current progression:

```text
0.1.0   initial repository
0.1.1   ESP32-S3 hardware baseline complete

0.2.x   microphone investigation and proof
0.2.6   first clearly intelligible Sipeed WAV; Phase 2 complete
0.2.7   dedicated continuous I2S acquisition task
0.2.8   level monitoring and PSRAM ring buffer
0.2.9   recovery and endurance testing
0.3.0   stable audio foundation complete

0.3.x   camera proof
0.4.0   camera subsystem stable

0.4.x   concurrent audio and camera work
0.5.0   concurrent subsystem stable

0.5.x   security event engine
0.6.0   security detection stable

0.6.x   cisOne integration
0.7.0   integrated Garage Sentinel

1.0.0   first stable deployed garage security node
```

# Worklog

| Date | Version | Commit name | Milestone / Notes |
|---|---|---|---|
| 2026-08-23 | `0.1.0` | `Initialize cisone-garage-sentinel repository` | Created the PlatformIO project structure, README, phase milestones, configuration layout, and initial ESP32-S3 baseline firmware. |
| 2026-08-24 | `0.1.1` | `Verify ESP32-S3 N16R8 hardware baseline` | Confirmed ESP32-S3 dual-core operation at 240 MHz, 16 MB flash, 8 MB PSRAM, successful 1 MiB PSRAM write/read test, stable serial output and stable heartbeat. Phase 1 complete. |
| 2026-08-25 | `0.2.0–0.2.4` | `Diagnose INMP441 I2S audio` | Verified I2S clocks, slot isolation, raw 32-bit capture and WAV transfer. Two INMP441 modules produced data but failed the intelligible-audio gate; raw analysis ruled out several software-format explanations. |
| 2026-09-07 | `0.2.5` | `Verify Sipeed I2S microphone data path` | Reused the 48 kHz stereo-slot diagnostic with the Sipeed I2S_Mic. Confirmed active SLOT A, acoustic response and 24-bit alignment with the low 8 bits unused. |
| 2026-09-07 | `0.2.6` | `Capture first intelligible Sipeed WAV` | Captured and transferred a 10-second 48 kHz mono WAV using `raw >> 8`; playback contained clearly intelligible speech. Phase 2 complete and Sipeed selected as the reference microphone. |
| 2026-09-08 | `0.2.7` | `Stabilize continuous I2S audio acquisition` | Added dedicated FreeRTOS audio acquisition task, live RMS/dBFS and health monitoring, bounded I2S reads and memory diagnostics. Completed approximately 90 minutes of continuous 48 kHz acquisition with zero read errors, zero timeouts, zero-length reads, and stable heap/PSRAM. |

Add one row for each meaningful tested commit rather than every minor edit.

Suggested commit naming style:

```text
Verify ESP32-S3 N16R8 hardware baseline
Diagnose INMP441 I2S audio
Verify Sipeed I2S microphone data path
Capture first intelligible Sipeed WAV
Add continuous I2S audio task
Add audio level monitoring and ring buffer
Verify audio recovery and endurance
Add camera still capture
Add concurrent audio and video tasks
Add acoustic event detection
Integrate cisOne event upload
```

# Current Status

**Current phase:** Phase 3 — Stable Audio Subsystem

**Completed:**

```text
Phase 1 — ESP32-S3 Hardware Baseline   PASS
Phase 2 — I2S Microphone Proof         PASS
```

**Reference microphone:** Sipeed I2S_Mic / MSM261S4030H0

**Current firmware:** `0.2.6`

Current immediate goal:

> Build version `0.2.7` with a dedicated continuous I2S acquisition task, bounded read timeouts and basic health counters, while preserving the known-good 48 kHz Sipeed microphone configuration and clearly intelligible audio.
