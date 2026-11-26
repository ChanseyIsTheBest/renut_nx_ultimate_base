#!/usr/bin/env python3
"""Read a frame capture (captures/frame_<n>_<s>s/, render/draw.cpp "full frame captures", s4ep2_frame_capture).
S4ep2-XBLA-nx's tools/s4_capture_read.py (github.com/FurryLilManChild/S4ep2-XBLA-nx, GPL-3.0, 0.5.1, its bbf7eb58);
the base's plume formats added to its table. Needs numpy and zstandard (pip).

  frame_capture_read.py images <capture folder> [--out DIR] [--only N,N,...]
      every img_*.bin.zst to PNG (values clamped to 0..1, and a second PNG scaled by the image's own maximum for float
      images) plus one stats line per image (min, max, mean per channel, NaN and Inf counts)
  frame_capture_read.py pixel <capture folder> <image> <x> <y> [--level L]
      the raw values of one texel
  (guest_*.bin.zst: a draw's vertex streams and indices as guest bytes, zstd only; index.txt names each)
  frame_capture_read.py draw <capture folder> <draw> [--vs-regs a-b] [--ps-regs a-b]
      one draw's binary record: the float constants it used (as floats), the device words asked for
"""
import argparse
import os
import re
import struct
import sys

import numpy as np
import zstandard

# plume::RenderFormat (plume_render_interface_types.h order).
FORMATS = """UNKNOWN R32G32B32A32_TYPELESS R32G32B32A32_FLOAT R32G32B32A32_UINT R32G32B32A32_SINT R32G32B32_TYPELESS
R32G32B32_FLOAT R32G32B32_UINT R32G32B32_SINT R16G16B16A16_TYPELESS R16G16B16A16_FLOAT R16G16B16A16_UNORM
R16G16B16A16_UINT R16G16B16A16_SNORM R16G16B16A16_SINT R32G32_TYPELESS R32G32_FLOAT R32G32_UINT R32G32_SINT
R8G8B8A8_TYPELESS R8G8B8A8_UNORM R8G8B8A8_UINT R8G8B8A8_SNORM R8G8B8A8_SINT B8G8R8A8_UNORM R16G16_TYPELESS
R16G16_FLOAT R16G16_UNORM R16G16_UINT R16G16_SNORM R16G16_SINT R32_TYPELESS D32_FLOAT D32_FLOAT_S8_UINT R32_FLOAT
R32_UINT R32_SINT R8G8_TYPELESS R8G8_UNORM R8G8_UINT R8G8_SNORM R8G8_SINT R16_TYPELESS R16_FLOAT D16_UNORM R16_UNORM
R16_UINT R16_SNORM R16_SINT R8_TYPELESS R8_UNORM R8_UINT R8_SNORM R8_SINT BC1_TYPELESS BC1_UNORM BC1_UNORM_SRGB
BC2_TYPELESS BC2_UNORM BC2_UNORM_SRGB BC3_TYPELESS BC3_UNORM BC3_UNORM_SRGB BC4_TYPELESS BC4_UNORM BC4_SNORM
BC5_TYPELESS BC5_UNORM BC5_SNORM BC6H_TYPELESS BC6H_UF16 BC6H_SF16 BC7_TYPELESS BC7_UNORM BC7_UNORM_SRGB S8_UINT
D24_UNORM_S8_UINT R10G10B10A2_UNORM R11G11B10_FLOAT R5G6B5_UNORM R5G5B5A1_UNORM R4G4B4A4_UNORM""".split()


def read_image(path):
    raw = zstandard.ZstdDecompressor().decompressobj().decompress(open(path, 'rb').read())
    end = raw.index(b'\n')
    head = raw[:end].decode().split()
    fields = dict(zip(head[1::2], head[2::2]))
    info = {k: int(v) for k, v in fields.items()}
    return info, raw[end + 1:]


def bc1_block_colors(c0, c1):
    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.float32)
    a, b = rgb(c0), rgb(c1)
    four = (c0 > c1)[..., None]
    p2 = np.where(four, (2 * a + b) / 3, (a + b) / 2)
    p3 = np.where(four, (a + 2 * b) / 3, 0)
    alpha3 = np.where(c0 > c1, 255, 0)
    cols = np.stack([a, b, p2, p3], -2)  # ..., 4, 3
    return cols, alpha3


def decode_bc(data, w, h, kind):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    size = 8 if kind in ('BC1', 'BC4') else 16
    blocks = np.frombuffer(data[:bw * bh * size], dtype=np.uint8).reshape(bh, bw, size)
    out = np.zeros((bh * 4, bw * 4, 4), np.float32)
    def color_part(b):
        c0 = b[..., 0].astype(np.uint32) | b[..., 1].astype(np.uint32) << 8
        c1 = b[..., 2].astype(np.uint32) | b[..., 3].astype(np.uint32) << 8
        idx = b[..., 4].astype(np.uint32) | b[..., 5].astype(np.uint32) << 8 | b[..., 6].astype(np.uint32) << 16 | b[..., 7].astype(np.uint32) << 24
        cols, alpha3 = bc1_block_colors(c0, c1)
        res = np.zeros((bh, bw, 4, 4, 4), np.float32)
        for i in range(16):
            sel = (idx >> (2 * i)) & 3
            px = np.take_along_axis(cols, sel[..., None, None].repeat(3, -1), -2)[..., 0, :]
            res[:, :, i // 4, i % 4, :3] = px
            res[:, :, i // 4, i % 4, 3] = np.where((sel == 3) & (kind == 'BC1'), alpha3, 255)
        return res
    def alpha_part(b):  # BC3/BC4 interpolated
        a0 = b[..., 0].astype(np.float32); a1 = b[..., 1].astype(np.float32)
        bits = np.zeros(b.shape[:-1], np.uint64)
        for k in range(6):
            bits |= b[..., 2 + k].astype(np.uint64) << np.uint64(8 * k)
        pal = [a0, a1]
        for k in range(1, 7):
            pal.append(np.where(a0 > a1, ((7 - k) * a0 + k * a1) / 7, 0))
        for k in range(1, 5):
            pal[1 + k] = np.where(a0 > a1, pal[1 + k], ((5 - k) * a0 + k * a1) / 5)
        pal[6] = np.where(a0 > a1, pal[6], 0); pal[7] = np.where(a0 > a1, pal[7], 255)
        pal = np.stack(pal, -1)
        res = np.zeros((bh, bw, 4, 4), np.float32)
        for i in range(16):
            sel = ((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64)
            res[:, :, i // 4, i % 4] = np.take_along_axis(pal, sel[..., None], -1)[..., 0]
        return res
    if kind == 'BC1':
        res = color_part(blocks)
    elif kind == 'BC2':
        res = color_part(blocks[..., 8:])
        ab = blocks[..., :8]
        for i in range(16):
            nib = (ab[..., i // 2] >> (4 * (i % 2))) & 15
            res[:, :, i // 4, i % 4, 3] = nib * 17
    elif kind == 'BC3':
        res = color_part(blocks[..., 8:])
        res[..., 3] = alpha_part(blocks[..., :8])
    elif kind == 'BC4':
        res = np.zeros((bh, bw, 4, 4, 4), np.float32)
        res[..., 0] = alpha_part(blocks); res[..., 3] = 255
    elif kind == 'BC5':
        res = np.zeros((bh, bw, 4, 4, 4), np.float32)
        res[..., 0] = alpha_part(blocks[..., :8]); res[..., 1] = alpha_part(blocks[..., 8:]); res[..., 3] = 255
    else:
        return None
    out = res.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:h, :w] / 255.0
    return out


def to_array(info, data):
    """The texels as float32 H x W x 4 (depth slices stacked vertically), and the format's name."""
    fmt = FORMATS[info['host_format']] if info['host_format'] < len(FORMATS) else str(info['host_format'])
    w, h, d = info['width'], info['height'] * info['depth'], 1
    n = w * h
    if fmt.startswith('BC'):
        return decode_bc(data, info['width'], info['height'], fmt[:3]), fmt
    def take(dtype, comps):
        a = np.frombuffer(data[:n * comps * np.dtype(dtype).itemsize], dtype=dtype).reshape(h, w, comps).astype(np.float32)
        return a
    if fmt in ('R16G16B16A16_FLOAT',):
        a = take(np.float16, 4)
    elif fmt in ('R32G32B32A32_FLOAT',):
        a = take(np.float32, 4)
    elif fmt in ('R16G16B16A16_UNORM',):
        a = take(np.uint16, 4) / 65535.0
    elif fmt in ('R8G8B8A8_UNORM', 'R8G8B8A8_TYPELESS', 'R8G8B8A8_UINT'):
        a = take(np.uint8, 4) / 255.0
    elif fmt == 'B8G8R8A8_UNORM':
        a = take(np.uint8, 4)[..., [2, 1, 0, 3]] / 255.0
    elif fmt in ('R32_FLOAT', 'D32_FLOAT'):
        a = np.repeat(take(np.float32, 1), 4, -1); a[..., 3] = 1
    elif fmt in ('R16_FLOAT',):
        a = np.repeat(take(np.float16, 1), 4, -1); a[..., 3] = 1
    elif fmt in ('R16G16_FLOAT',):
        a = take(np.float16, 2); a = np.concatenate([a, np.zeros_like(a[..., :1]), np.ones_like(a[..., :1])], -1)
    elif fmt in ('R32G32_FLOAT',):
        a = take(np.float32, 2); a = np.concatenate([a, np.zeros_like(a[..., :1]), np.ones_like(a[..., :1])], -1)
    elif fmt in ('R8_UNORM',):
        a = np.repeat(take(np.uint8, 1) / 255.0, 4, -1); a[..., 3] = 1
    elif fmt in ('R8G8_UNORM',):
        a = take(np.uint8, 2) / 255.0; a = np.concatenate([a, np.zeros_like(a[..., :1]), np.ones_like(a[..., :1])], -1)
    elif fmt in ('R16_UNORM', 'D16_UNORM'):
        a = np.repeat(take(np.uint16, 1) / 65535.0, 4, -1); a[..., 3] = 1
    elif fmt == 'D24_UNORM_S8_UINT':
        u = np.frombuffer(data[:n * 4], dtype=np.uint32).reshape(h, w)
        depth = (u & 0xFFFFFF).astype(np.float32) / 16777215.0
        a = np.stack([depth, depth, depth, (u >> 24).astype(np.float32) / 255.0], -1)
    elif fmt in ('R32_UINT',):
        a = np.repeat(take(np.uint32, 1), 4, -1); a[..., 3] = 1
    else:
        return None, fmt
    return a, fmt


def save_png(path, rgb):
    from PIL import Image
    img = np.clip(np.nan_to_num(rgb, nan=0, posinf=1, neginf=0), 0, 1)
    Image.fromarray((img * 255 + 0.5).astype(np.uint8), 'RGBA' if img.shape[-1] == 4 else 'RGB').save(path)


def cmd_images(args):
    os.makedirs(args.out, exist_ok=True)
    only = set(int(x) for x in args.only.split(',')) if args.only else None
    scan = os.path.join(args.folder, 'scanout.rgba.zst')
    if os.path.exists(scan):
        raw = zstandard.ZstdDecompressor().decompressobj().decompress(open(scan, 'rb').read())
        end = raw.index(b'\n')
        f = raw[:end].decode().split()
        w, h, stride = int(f[2]), int(f[4]), int(f[6])
        px = np.frombuffer(raw[end + 1:end + 1 + stride * h], np.uint8).reshape(h, stride)[:, :w * 4].reshape(h, w, 4)
        save_png(os.path.join(args.out, 'scanout.png'), px[..., :3] / 255.0)
        print('scanout:', raw[:end].decode())
    names = sorted(f for f in os.listdir(args.folder) if f.startswith('img_') and f.endswith('.bin.zst'))
    for name in names:
        m = re.match(r'img_(\d+)_l(\d+)_a(\d+)', name)
        if only and int(m.group(1)) not in only:
            continue
        info, data = read_image(os.path.join(args.folder, name))
        a, fmt = to_array(info, data)
        base = name[:-len('.bin.zst')]
        if a is None:
            print(f'{base}: {info["width"]}x{info["height"]}x{info["depth"]} {fmt}: no decoder')
            continue
        finite = np.isfinite(a)
        stats = []
        for c in range(4):
            v = a[..., c][finite[..., c]]
            stats.append(f'{"RGBA"[c]} {v.min():.4g}..{v.max():.4g} mean {v.mean():.4g}' if v.size else f'{"RGBA"[c]} none')
        print(f'{base}: {info["width"]}x{info["height"]}x{info["depth"]} {fmt}; ' + '; '.join(stats) +
              f'; NaN {int(np.isnan(a).sum())} Inf {int(np.isinf(a).sum())}')
        rgb = a[..., :3]
        save_png(os.path.join(args.out, base + '.png'), rgb)
        if 'FLOAT' in fmt:
            peak = np.nanmax(np.where(np.isfinite(rgb), rgb, 0))
            if peak > 1.0:
                save_png(os.path.join(args.out, base + '_scaled.png'), rgb / peak)
        if not fmt.startswith('D') and (a[..., 3] != 1).any():
            save_png(os.path.join(args.out, base + '_alpha.png'), np.repeat(a[..., 3:4], 3, -1))


def cmd_pixel(args):
    name = [f for f in os.listdir(args.folder) if f.startswith(f'img_{args.image:04}_l{args.level}_a0')][0]
    info, data = read_image(os.path.join(args.folder, name))
    a, fmt = to_array(info, data)
    print(fmt, a[args.y, args.x])


def cmd_draw(args):
    raw = zstandard.ZstdDecompressor().decompressobj().decompress(open(os.path.join(args.folder, 'draws.bin.zst'), 'rb').read())
    off = 0
    head = struct.Struct('<4sIIIIIQQ')
    while off < len(raw):
        magic, draw, dev, consts, shared, key, vs, ps = head.unpack_from(raw, off)
        assert magic == b'S4DR', magic
        body = off + head.size
        if draw == args.draw:
            print(f'draw {draw}: vs {vs:016X} ps {ps:016X} (device {dev} bytes, shared {shared}, key {key})')
            vsc = body + dev
            psc = vsc + consts
            def regs(start, spec, label):
                a, b = (int(x) for x in spec.split('-'))
                for r in range(a, b + 1):
                    words = struct.unpack_from('>4f', raw, start + r * 16)
                    print(f'  {label} c{r}: ' + ' '.join(f'{x:.6g}' for x in words))
            regs(vsc, args.vs_regs, 'vs')
            regs(psc, args.ps_regs, 'ps')
            return
        off = body + dev + 2 * consts + shared + key
    print('no such draw')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('images'); p.add_argument('folder'); p.add_argument('--out', default='png'); p.add_argument('--only')
    p.set_defaults(fn=cmd_images)
    p = sub.add_parser('pixel'); p.add_argument('folder'); p.add_argument('image', type=int); p.add_argument('x', type=int)
    p.add_argument('y', type=int); p.add_argument('--level', type=int, default=0); p.set_defaults(fn=cmd_pixel)
    p = sub.add_parser('draw'); p.add_argument('folder'); p.add_argument('draw', type=int)
    p.add_argument('--vs-regs', default='0-15'); p.add_argument('--ps-regs', default='0-15'); p.set_defaults(fn=cmd_draw)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
