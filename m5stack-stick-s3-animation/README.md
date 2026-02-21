# Bad Apple on M5Stack StickS3

Bad Apple!! music video player for **M5Stack StickS3** (ESP32-S3) with interactive effects.

180x135 1-bit video with audio playback, gyroscope-based rotation, random colors, and glitch effects.

## Hardware

- M5Stack StickS3 (ESP32-S3, 8 MB flash, 8 MB PSRAM)
- ST7789 135x240 LCD
- ES8311 speaker (I2S)
- MPU6886 / BMI270 IMU

## Features

| Input | Effect |
|---|---|
| **BtnA** short press | Invert colors |
| **BtnA** hold 600 ms | Pause / resume |
| **BtnB** press | Random contrasting color pair |
| **Tilt device** | Video rotates smoothly to stay upright |
| **Shake device** | Glitch effect for ~8 frames |

## Build

### Prerequisites

- [PlatformIO](https://platformio.org/)
- Python 3 with [Pillow](https://pillow.readthedocs.io/) (`pip install Pillow`)
- [ffmpeg](https://ffmpeg.org/) (install via `winget install ffmpeg` on Windows)

### 1. Prepare data

Place your Bad Apple video as an `.mp4` file in the project root, then run:

```bash
python tools/build_data.py "Bad Apple.mp4" --width 180 --height 135 --fps 15
```

This generates `data/bad_apple.bin` (video, ~2.4 MB) and `data/bad_apple_audio.raw` (audio, ~1.7 MB).

The script auto-detects ffmpeg installed via winget.

### 2. Upload data to LittleFS

```bash
pio run -t uploadfs
```

### 3. Build and flash firmware

```bash
pio run -t upload
```

### 4. Monitor serial output

```bash
pio device monitor
```

## Data format

### Video (`bad_apple.bin`)

```
Header (12 bytes):
  uint16  width
  uint16  height
  uint32  total_frames
  uint16  fps
  uint16  flags (reserved)

Frame index (total_frames * 4 bytes):
  uint32  offset[]   -- byte offset of each frame in the data section

Frame data:
  Per frame: bit-level RLE encoded 1-bit image
    uint8   first_bit          -- value of the first run (0 or 1)
    uint16  run_lengths[]      -- alternating run lengths (LE)
```

Bit-level RLE achieves ~25% compression ratio on Bad Apple (vs raw 1-bit).

### Audio (`bad_apple_audio.raw`)

Raw unsigned 8-bit PCM, 8000 Hz, mono.

## Partition layout

Custom partition table (no OTA) to maximize data storage:

| Name | Size |
|---|---|
| app | 2 MB |
| LittleFS | ~5.9 MB |

## Project structure

```
src/main.cpp          -- firmware (video decode, audio, effects, IMU)
tools/build_data.py   -- data preparation script (ffmpeg + bit-RLE)
partitions.csv        -- custom flash partition table
platformio.ini        -- PlatformIO config
```

## License

MIT
