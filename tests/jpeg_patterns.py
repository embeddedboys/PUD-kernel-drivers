# SPDX-License-Identifier: GPL-2.0-only
"""Generate deterministic JPEG validation fixtures, without test verdicts."""

import argparse
from pathlib import Path
import random

from PIL import Image, ImageDraw, ImageFont

SIZE = (800, 480)


def gradient():
    width, height = SIZE
    pixels = [
        (x * 255 // (width - 1), y * 255 // (height - 1),
         (x + y) * 255 // (width + height - 2))
        for y in range(height) for x in range(width)
    ]
    image = Image.new("RGB", SIZE)
    image.putdata(pixels)
    return image


def text_lines(font):
    image = Image.new("RGB", SIZE, "white")
    draw = ImageDraw.Draw(image)
    for y in range(0, 440, 40):
        text = f"USB HS JPEG 800x480 | row {y // 40:02d} | AaBb 0123456789"
        draw.text((15, y + 5), text, fill="black", font=font)
        draw.line((10, y + 35, 790, y + 35), fill="black", width=1)
    for x in range(0, 800, 4):
        draw.line((x, 440, x, 479), fill="black")
    return image


def composite(background, photo, font):
    image = background.copy()
    image.paste(photo.resize((480, 276)), (300, 160))
    draw = ImageDraw.Draw(image)
    draw.rectangle((10, 10, 790, 110), fill="#17243a")
    draw.text((30, 28), "PUD DRM / USB HS / HARDWARE JPEG", fill="white", font=font)
    draw.text((30, 65), "800 x 480 - complex frame verification",
              fill="#55ddff", font=font)
    for row, color in enumerate(("#ff0000", "#00ff00", "#0000ff", "#ffffff", "#000000")):
        draw.rectangle((20, 150 + row * 50, 270, 185 + row * 50), fill=color)
    draw.text((25, 425), "RED / GREEN / BLUE / WHITE / BLACK", fill="white",
              font=ImageFont.truetype("DejaVuSans.ttf", 13))
    return image


def generate(output, photo_path):
    output.mkdir(parents=True, exist_ok=True)
    font = ImageFont.truetype("DejaVuSans.ttf", 20)
    background = gradient()
    with Image.open(photo_path) as source:
        photo = source.convert("RGB").resize(SIZE, Image.Resampling.LANCZOS)
    noise = random.Random(17).randbytes(SIZE[0] * SIZE[1] * 3)
    fixtures = {
        "gradient": background,
        "text-lines": text_lines(font),
        "photo": photo,
        "noise": Image.frombytes("RGB", SIZE, noise),
        "composite": composite(background, photo, font),
    }
    for name, image in fixtures.items():
        image.save(output / (name + ".png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--photo", type=Path, required=True)
    args = parser.parse_args()
    generate(args.output, args.photo)
    print(args.output)


if __name__ == "__main__":
    main()
