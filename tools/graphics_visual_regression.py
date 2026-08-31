#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import argparse
import math
import struct
import sys
from pathlib import Path


def read_bmp(path):
    data = Path(path).read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP file")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError(f"{path}: unsupported BMP header")

    width = struct.unpack_from("<i", data, 18)[0]
    signed_height = struct.unpack_from("<i", data, 22)[0]
    planes = struct.unpack_from("<H", data, 26)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if width <= 0 or signed_height == 0 or planes != 1 or bpp != 32 or compression != 0:
        raise ValueError(f"{path}: expected uncompressed 32-bit BMP")

    height = abs(signed_height)
    top_down = signed_height < 0
    stride = width * 4
    required = pixel_offset + stride * height
    if required > len(data):
        raise ValueError(f"{path}: truncated pixel data")

    rgb = bytearray(width * height * 3)
    luma = bytearray(width * height)
    for y in range(height):
        source_y = y if top_down else height - 1 - y
        src = pixel_offset + source_y * stride
        dst = y * width * 3
        ldst = y * width
        for x in range(width):
            b = data[src + x * 4 + 0]
            g = data[src + x * 4 + 1]
            r = data[src + x * 4 + 2]
            base = dst + x * 3
            rgb[base + 0] = r
            rgb[base + 1] = g
            rgb[base + 2] = b
            luma[ldst + x] = (77 * r + 150 * g + 29 * b) >> 8
    return width, height, rgb, luma


def block_ssim(a, b, width, height, block=8):
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    total = 0.0
    blocks = 0
    for y0 in range(0, height, block):
        bh = min(block, height - y0)
        for x0 in range(0, width, block):
            bw = min(block, width - x0)
            n = bw * bh
            if n <= 1:
                continue
            sum_a = sum_b = 0.0
            sum_aa = sum_bb = sum_ab = 0.0
            for y in range(y0, y0 + bh):
                row = y * width
                for x in range(x0, x0 + bw):
                    av = float(a[row + x])
                    bv = float(b[row + x])
                    sum_a += av
                    sum_b += bv
                    sum_aa += av * av
                    sum_bb += bv * bv
                    sum_ab += av * bv
            mean_a = sum_a / n
            mean_b = sum_b / n
            variance_a = max(0.0, (sum_aa - n * mean_a * mean_a) / (n - 1))
            variance_b = max(0.0, (sum_bb - n * mean_b * mean_b) / (n - 1))
            covariance = (sum_ab - n * mean_a * mean_b) / (n - 1)
            numerator = (2.0 * mean_a * mean_b + c1) * (2.0 * covariance + c2)
            denominator = (mean_a * mean_a + mean_b * mean_b + c1) * (variance_a + variance_b + c2)
            total += numerator / denominator if denominator != 0.0 else 1.0
            blocks += 1
    return total / blocks if blocks else 1.0


def compare(reference, capture, pixel_threshold):
    rw, rh, rrgb, rluma = read_bmp(reference)
    cw, ch, crgb, cluma = read_bmp(capture)
    if (rw, rh) != (cw, ch):
        raise ValueError(f"dimension mismatch: reference={rw}x{rh}, capture={cw}x{ch}")
    total_channels = len(rrgb)
    total_pixels = rw * rh
    absolute_sum = 0
    squared_sum = 0.0
    maximum = 0
    mismatched_pixels = 0
    for pixel in range(total_pixels):
        base = pixel * 3
        pixel_mismatch = False
        for channel in range(3):
            diff = abs(int(rrgb[base + channel]) - int(crgb[base + channel]))
            absolute_sum += diff
            squared_sum += diff * diff
            maximum = max(maximum, diff)
            if diff > pixel_threshold:
                pixel_mismatch = True
        if pixel_mismatch:
            mismatched_pixels += 1
    mae = absolute_sum / total_channels if total_channels else 0.0
    mse = squared_sum / total_channels if total_channels else 0.0
    psnr = float("inf") if mse == 0.0 else 20.0 * math.log10(255.0 / math.sqrt(mse))
    mismatch_percent = 100.0 * mismatched_pixels / total_pixels if total_pixels else 0.0
    ssim = block_ssim(rluma, cluma, rw, rh)
    return {"width": rw, "height": rh, "mae": mae, "max_abs_error": maximum,
            "mismatch_percent": mismatch_percent, "psnr_db": psnr, "ssim": ssim}


def main():
    parser = argparse.ArgumentParser(description="Compare Chimera Graphics reference and capture BMPs.")
    parser.add_argument("reference")
    parser.add_argument("capture")
    parser.add_argument("--pixel-threshold", type=int, default=2)
    parser.add_argument("--max-mismatch-percent", type=float, default=0.10)
    parser.add_argument("--ssim-threshold", type=float, default=0.995)
    args = parser.parse_args()
    try:
        metrics = compare(args.reference, args.capture, max(0, min(255, args.pixel_threshold)))
    except (OSError, ValueError) as exc:
        print(f"error={exc}")
        return 2
    psnr_text = "inf" if math.isinf(metrics["psnr_db"]) else f'{metrics["psnr_db"]:.6f}'
    print(f'width={metrics["width"]}')
    print(f'height={metrics["height"]}')
    print(f'mae={metrics["mae"]:.6f}')
    print(f'max_abs_error={metrics["max_abs_error"]}')
    print(f'mismatch_percent={metrics["mismatch_percent"]:.6f}')
    print(f'psnr_db={psnr_text}')
    print(f'ssim={metrics["ssim"]:.9f}')
    passed = metrics["mismatch_percent"] <= args.max_mismatch_percent and metrics["ssim"] >= args.ssim_threshold
    print(f'status={"PASS" if passed else "FAIL"}')
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
