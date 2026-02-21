#!/usr/bin/env python3
"""Build Bad Apple data files for M5Stack StickS3.

Generates:
  data/bad_apple.bin       — video (header + frame index + per-frame bit-RLE)
  data/bad_apple_audio.raw — unsigned 8-bit PCM, 8kHz, mono

Bit-RLE format per frame:
  uint8_t  first_bit       — value of the first run (0 or 1)
  uint16_t run_lengths[]   — alternating run lengths (LE), until all pixels consumed

Usage:
  python tools/build_data.py "video.mp4" --width 180 --height 135 --fps 15
"""
import os
import sys
import argparse
import shutil
import subprocess
import struct
from PIL import Image

# Find ffmpeg: check PATH, then known winget location
FFMPEG = 'ffmpeg'
_winget_ffmpeg = os.path.expandvars(
    r'%LOCALAPPDATA%\Microsoft\WinGet\Packages'
)
if os.path.isdir(_winget_ffmpeg):
    for d in os.listdir(_winget_ffmpeg):
        if 'FFmpeg' in d:
            candidate = os.path.join(_winget_ffmpeg, d)
            for root, dirs, files in os.walk(candidate):
                if 'ffmpeg.exe' in files:
                    FFMPEG = os.path.join(root, 'ffmpeg.exe')
                    break


def image_to_bits(img, width, height, threshold=128):
    """Convert image to flat list of 0/1 values (row-major)."""
    img = img.convert('L').resize((width, height))
    pixels = img.load()
    bits = []
    for y in range(height):
        for x in range(width):
            bits.append(1 if pixels[x, y] < threshold else 0)
    return bits


def bit_rle_compress(bits):
    """Compress a flat bit list with bit-level RLE.

    Returns bytes: first_bit(uint8) + run_lengths(uint16_LE each).
    Runs alternate between 0 and 1 starting with first_bit.
    """
    if not bits:
        return b'\x00'
    out = bytearray()
    first_bit = bits[0]
    out.append(first_bit)
    run = 1
    for i in range(1, len(bits)):
        if bits[i] == bits[i - 1]:
            run += 1
            # uint16 max = 65535; split if exceeded
            if run == 65535:
                out.extend(struct.pack('<H', run))
                out.extend(struct.pack('<H', 0))  # zero-length run for opposite bit
                run = 0
        else:
            out.extend(struct.pack('<H', run))
            run = 1
    if run > 0:
        out.extend(struct.pack('<H', run))
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description='Build Bad Apple data files')
    p.add_argument('input', help='Input video file (mp4)')
    p.add_argument('--width', type=int, default=180)
    p.add_argument('--height', type=int, default=135)
    p.add_argument('--fps', type=int, default=15)
    p.add_argument('--audio-rate', type=int, default=8000,
                   help='Audio sample rate (Hz)')
    p.add_argument('--tmp', default='tmp_frames')
    p.add_argument('--data-dir', default='data')
    args = p.parse_args()

    total_pixels = args.width * args.height

    os.makedirs(args.data_dir, exist_ok=True)

    # --- Extract frames ---
    if os.path.exists(args.tmp):
        shutil.rmtree(args.tmp)
    os.makedirs(args.tmp)

    print(f'Extracting frames at {args.width}x{args.height} @ {args.fps}fps...')
    subprocess.check_call([
        FFMPEG, '-y', '-i', args.input,
        '-vf', f'scale={args.width}:{args.height}',
        '-r', str(args.fps),
        os.path.join(args.tmp, 'frame_%06d.png')
    ])

    files = sorted(f for f in os.listdir(args.tmp) if f.endswith('.png'))
    frame_count = len(files)
    print(f'Extracted {frame_count} frames')

    # --- Build video binary with bit-level RLE ---
    print('Packing frames with per-frame bit-RLE...')
    compressed_frames = []
    total_rle = 0
    for idx, fn in enumerate(files):
        if idx % 500 == 0:
            print(f'  Frame {idx}/{frame_count}...')
        img = Image.open(os.path.join(args.tmp, fn))
        bits = image_to_bits(img, args.width, args.height)
        compressed = bit_rle_compress(bits)
        compressed_frames.append(compressed)
        total_rle += len(compressed)

    raw_bits = frame_count * (total_pixels + 7) // 8
    print(f'  Bit-RLE total: {total_rle:,} bytes (raw 1-bit would be {raw_bits:,}, '
          f'ratio {100*total_rle/raw_bits:.1f}%)')

    # Calculate frame offsets (relative to start of frame data section)
    offset = 0
    frame_offsets = []
    for cf in compressed_frames:
        frame_offsets.append(offset)
        offset += len(cf)

    video_path = os.path.join(args.data_dir, 'bad_apple.bin')
    with open(video_path, 'wb') as out:
        # Header: 12 bytes (aligned to 4)
        #   uint16 width, uint16 height, uint32 frames, uint16 fps, uint16 flags
        out.write(struct.pack('<HHIHH',
                              args.width, args.height,
                              frame_count, args.fps, 0))

        # Frame index: frame_count * 4 bytes
        for off in frame_offsets:
            out.write(struct.pack('<I', off))

        # Frame data
        for cf in compressed_frames:
            out.write(cf)

    video_size = os.path.getsize(video_path)
    print(f'Video: {video_path} — {video_size:,} bytes ({video_size/1024/1024:.2f} MB)')

    # --- Extract audio as unsigned 8-bit PCM ---
    audio_path = os.path.join(args.data_dir, 'bad_apple_audio.raw')
    print(f'Extracting audio at {args.audio_rate}Hz, unsigned 8-bit mono...')
    subprocess.check_call([
        FFMPEG, '-y', '-i', args.input,
        '-vn',
        '-ac', '1',
        '-ar', str(args.audio_rate),
        '-f', 'u8',
        '-acodec', 'pcm_u8',
        audio_path
    ])

    audio_size = os.path.getsize(audio_path)
    print(f'Audio: {audio_path} — {audio_size:,} bytes ({audio_size/1024/1024:.2f} MB)')

    total = video_size + audio_size
    partition_size = 0x5F0000  # from partitions.csv
    usable = int(partition_size * 0.95)  # LittleFS overhead
    print(f'\nTotal data: {total:,} bytes ({total/1024/1024:.2f} MB)')
    print(f'LittleFS usable: ~{usable:,} bytes ({usable/1024/1024:.2f} MB)')
    if total > usable:
        print('WARNING: data may not fit! Consider --audio-rate 6000 or smaller resolution')
    else:
        print('OK — fits in partition.')

    # Cleanup
    shutil.rmtree(args.tmp)
    print('Done. Cleaned up tmp_frames.')


if __name__ == '__main__':
    main()
