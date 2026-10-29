#!/usr/bin/env python3
"""The game's controller error banner (its insanity effect 29), redone for each
machine the recomp runs on: res/insanity/controller-error-<platform>.png.

The game's own is a 596x50 CMPR picture in a UI TPL it keeps loaded (white on
black, two centred lines, stored upside down as the game draws it):
"An error has occurred reading the Nintendo GameCube(TM) Controller." /
"Please insert Controller into Nintendo GameCube(TM) Controller Socket 1."
texpack.cpp puts this machine's in its place (tex1_596x50_f238cd2e013b429a_14)
whether or not an HD texture pack is installed. Drawn here at four times the
size, in Fira Sans, the same way up as the game's.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FONT = os.path.join(ROOT, 'res', 'fonts', 'FiraSans-Regular.ttf')
OUT = os.path.join(ROOT, 'res', 'insanity')
W, H = 596 * 4, 50 * 4

TEXT = {
    'ps5': ('An error has occurred reading the DUALSENSE™ wireless controller.',
            'Please connect the controller to the PS5™ console using a USB cable.'),
    'windows': ('An error has occurred reading the controller.',
                'Please connect the controller to USB Port 1.'),
    'linux': ('An error has occurred reading the controller.',
              'Please connect the controller to USB Port 1.'),
    'deck': ('An error has occurred reading the Steam Deck™ controls.',
             'Please restart your Steam Deck™ to continue.'),
    'mac': ('An error has occurred reading the game controller.',
            'Please reconnect the controller in System Settings > Bluetooth.'),
}


def banner(lines):
    img = Image.new('RGBA', (W, H), (0, 0, 0, 255))
    draw = ImageDraw.Draw(img)
    # One size for both lines: as the game's (about 19 of its 50 lines a
    # letter), smaller only where a line would not fit the width.
    size = 76
    while size > 20:
        font = ImageFont.truetype(FONT, size)
        if max(draw.textlength(line, font=font) for line in lines) <= W * 0.96:
            break
        size -= 2
    gap = H / 2
    for i, line in enumerate(lines):
        y = gap * (i + 0.5)
        draw.text((W / 2, y), line, font=font, fill=(255, 255, 255, 255), anchor='mm')
    return img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)


def main():
    os.makedirs(OUT, exist_ok=True)
    for platform, lines in TEXT.items():
        path = os.path.join(OUT, 'controller-error-%s.png' % platform)
        banner(lines).save(path, optimize=True)
        print(path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
