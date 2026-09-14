"""
ppm2png.py - Convert the runtime's framebuffer dump to a PNG.

CIV_SHOTS writes a binary PPM because the runtime is plain C with no image
library to link against. This turns one into a PNG using nothing but the standard
library, so a screenshot is two commands and no dependencies:

    CIV_SHOTS=work/shots build/Release/civ.exe
    python tools/ppm2png.py work/shots docs/screenshots/title.png
"""
import struct
import sys
import zlib


def read_ppm(path):
    """Read a binary PPM (P6). Header fields are whitespace-separated, and a
    comment runs to end of line -- which is why this is a token scan and not a
    fixed-size read."""
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b'P6':
        raise SystemExit(f'{path}: not a binary PPM')
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload
                + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

    # Each PNG scanline is prefixed with its filter type; 0 is "none".
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(f'usage: {sys.argv[0]} <in.ppm> <out.png>')
    width, height, pixels = read_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, pixels)
    print(f'{sys.argv[2]}: {width}x{height}')
