#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): copied from sasrt-nx's tools/hevc_usm_pack.py (credit sasrt-nx, rr6-nx, culdcept-nx) for
this game's USMs (CRI Movie 2.04: Resource/Movie/{Logos,Gameplay,Credits}.usm, the same CRID / @SFV / @SFA chunks);
the hook that plays them here is app/src/ssasr_usm_movies.cpp.

sasrt-nx (2026-11-21, the user: "please compress all the videos and audio", taking rr6-nx's and culdcept-nx's work):
makes NAME.hevc from one of SASRT's CRI USM movies NAME (Data/Movies/*.usm: @SFV chunks of MPEG-1 video, @SFA ADX
audio). rr6-nx's tools/hevc_sfd_pack.py (2026-11-21, credit rr6-nx) with a USM demux in place of its MPEG program
stream's: sdk/include/rex/filesystem/sfd_hevc.h has the format (any container: segments of stored bytes, zeros and tag
bytes) and app/src/cpu/sasrt_usm_ffmpeg.cpp the hook that plays it (the slice loop finds the tag and writes the H.265
picture of that display index into the coded picture's planes). Every byte of the USM but the pictures' slice data is
kept (CRI's chunk headers, padding, the audio, the video's sequence, GOP and picture headers); each picture's slice data
becomes a slice header (quantiser 1), a 10-byte tag and zeros. The pictures are decoded by ffmpeg in display order and
encoded as H.265 without B frames (a keyframe every --gop frames, with its parameter sets), at --fraction of the MPEG-1
video's own bit rate.

  python -P tools/hevc_usm_pack.py IN.usm OUT.hevc [--fraction 0.35] [--gop 120] [--encoder libx265|hevc_nvenc]
      [--ffmpeg C:/ffmpeg/ffmpeg.exe]
  python -P tools/hevc_usm_pack.py --check IN.usm OUT.hevc [--psnr-every 30]
      the movie the port shows the game (every stored byte as in IN, every tag where a picture's slices begin) and the
      H.265's PSNR against ffmpeg's MPEG-1 decode, every Nth frame
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

MAGIC = b'RXSFDH01'
TAG_BYTES = 10
TAG_MAGIC = bytes([0xA5, 0x5A, 0xC3])
QUANT_BYTE = 0x08  # quantiser_scale 1, extra_bit_slice 0


def make_tag(movie_id, index):
    t = bytearray(TAG_MAGIC)
    for v in (movie_id, index):
        t += bytes([0x80 | ((v >> 14) & 0x7F), 0x80 | ((v >> 7) & 0x7F), 0x80 | (v & 0x7F)])
    t.append(0x80 | (sum(t[3:9]) & 0x7F))
    return bytes(t)


# ---- the USM ------------------------------------------------------------------------------------------------------

def demux(d):
    """The video ES and where its bytes lie in the file: es (bytes), runs [(es offset, file offset, length)]. A USM is
    CRI chunks back to back (big-endian): signature, u32 size of the rest, u8 ?, u8 payload offset (from +8), u16 padding
    at the end, u8 channel, 2 bytes, u8 data type (0 stream data, 1 header, 2 section end, 3 metadata), ...; the video
    ES is the payloads of the @SFV data chunks (type 0) of channel 0, in order."""
    n = len(d)
    i = 0
    es = bytearray()
    runs = []
    while i + 16 <= n:
        sig = d[i:i + 4]
        size = struct.unpack('>I', d[i + 4:i + 8])[0]
        if i + 8 + size > n:
            raise SystemExit(f'a chunk at {i} runs past the end')
        poff, pad = d[i + 9], struct.unpack('>H', d[i + 10:i + 12])[0]
        ch, dtype = d[i + 12], d[i + 15]
        if sig == b'@SFV' and dtype == 0 and ch == 0:
            p, end = i + 8 + poff, i + 8 + size - pad
            # ssasr-nx (2026-11-25): CRI Movie 2.04's video payloads (Sonic & SEGA All-Stars Racing) start with 64 zero
            # bytes before the MPEG data (every one of the 12,020 in its three USMs); they stay stored bytes, not ES.
            if end - p > 67 and d[p:p + 64] == bytes(64) and d[p + 64:p + 67] == b'\x00\x00\x01':
                p += 64
            if end > p:
                runs.append((len(es), p, end - p))
                es += d[p:end]
        i += 8 + size
    if i != n:
        raise SystemExit(f'the chunks end at {i}, the file at {n}')
    if not es.startswith(b'\x00\x00\x01\xb3'):
        raise SystemExit('the video does not start with a sequence header (encrypted?)')
    return bytes(es), runs


def pictures(es):
    """The coded pictures: [(first slice start code, end of its slices, display index)], and the GOP count."""
    starts = []
    p = es.find(b'\x00\x00\x01')
    while p >= 0:
        if p + 3 < len(es):
            starts.append((p, es[p + 3]))
        p = es.find(b'\x00\x00\x01', p + 3)
    pics = []
    gop_base, in_gop, gops = 0, 0, 0
    cur = None
    for s, c in starts:
        if c == 0x00:
            if cur and cur['slice0'] is not None and cur['end'] is None:
                cur['end'] = s
            tref = (es[s + 4] << 2) | (es[s + 5] >> 6)
            cur = {'tref': tref, 'base': gop_base, 'slice0': None, 'end': None}
            pics.append(cur)
            in_gop += 1
        elif 0x01 <= c <= 0xAF:
            if cur and cur['slice0'] is None:
                cur['slice0'] = s
        else:
            if cur and cur['slice0'] is not None and cur['end'] is None:
                cur['end'] = s
            if c == 0xB8:  # a GOP: its pictures' temporal references count from the pictures before it
                gop_base += in_gop
                in_gop = 0
                gops += 1
    if cur and cur['slice0'] is not None and cur['end'] is None:
        cur['end'] = len(es)
    out = [(q['slice0'], q['end'], q['base'] + q['tref']) for q in pics]
    if any(s is None for s, _, _ in out):
        raise SystemExit('a picture without slices')
    disp = sorted(x[2] for x in out)
    if disp != list(range(len(out))):
        raise SystemExit('the display indices are not 0..N-1 (an unexpected GOP structure)')
    return out, gops


def es_to_file(runs, es_off, length):
    """The file ranges of ES bytes [es_off, es_off + length)."""
    import bisect
    keys = [r[0] for r in runs]
    k = bisect.bisect_right(keys, es_off) - 1
    out = []
    while length > 0 and k < len(runs):
        e0, f0, ln = runs[k]
        a = max(es_off, e0)
        b = min(es_off + length, e0 + ln)
        if a < b:
            out.append((f0 + a - e0, b - a))
            length -= b - a
            es_off = b
        k += 1
    return out


# ---- H.265 ----------------------------------------------------------------------------------------------------------

FRAME_RATES = {1: 24000 / 1001, 2: 24.0, 3: 25.0, 4: 30000 / 1001, 5: 30.0, 6: 50.0, 7: 60000 / 1001, 8: 60.0}


def probe(es):
    """Width, height and frame rate from the first sequence header."""
    w = (es[4] << 4) | (es[5] >> 4)
    h = ((es[5] & 0xF) << 8) | es[6]
    return w, h, FRAME_RATES[es[7] & 0xF]


def encode(ffmpeg, src, dst, encoder, bitrate, gop, fps):
    vf = []
    if encoder == 'libx265':
        enc = ['-c:v', 'libx265', '-preset', 'slow', '-b:v', str(bitrate), '-x265-params',
               f'bframes=0:keyint={gop}:min-keyint={gop}:scenecut=0:open-gop=0:repeat-headers=1:aud=1:info=0:'
               f'log-level=error']
    else:
        enc = ['-c:v', 'hevc_nvenc', '-preset', 'p7', '-tune', 'hq', '-rc', 'vbr', '-b:v', str(bitrate), '-maxrate',
               str(bitrate * 2), '-bf', '0', '-g', str(gop), '-forced-idr', '1', '-no-scenecut', '1',
               '-bsf:v', 'hevc_metadata=aud=insert']
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'mpegvideo', '-i', src, '-map', '0:v:0', '-an',
           '-fps_mode',
           'passthrough', '-pix_fmt', 'yuv420p', *enc, '-f', 'hevc', dst]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=7200)
    if p.returncode:
        raise SystemExit('ffmpeg failed: ' + p.stderr[-2000:])


def access_units(stream):
    """An Annex B H.265 stream cut at its access unit delimiters: [(bytes, keyframe)]."""
    nals = []
    p = stream.find(b'\x00\x00\x01')
    while p >= 0:
        start = p - 1 if p > 0 and stream[p - 1] == 0 else p
        nals.append((start, (stream[p + 3] >> 1) & 0x3F))
        p = stream.find(b'\x00\x00\x01', p + 3)
    aus, cur_start, key = [], None, False
    for k, (start, typ) in enumerate(nals):
        if typ == 35:  # AUD
            if cur_start is not None:
                aus.append([cur_start, start, key])
            cur_start, key = start, False
        if 16 <= typ <= 21:
            key = True
    if cur_start is not None:
        aus.append([cur_start, len(stream), key])
    return [(stream[a:b], k) for a, b, k in aus]


# ---- the .hevc file ------------------------------------------------------------------------------------------------

def build(d, runs, pics, frames, width, height, fps):
    n = len(d)
    kind = np.zeros(n, np.uint8)  # 0 stored, 1 zero, 2 tag
    view = bytearray(d)
    tags = []  # (file offset, coded picture, tag byte index)
    untagged = 0
    for k, (s0, end, disp) in enumerate(pics):
        region = end - (s0 + 4)
        if region < 1 + TAG_BYTES:
            untagged += 1
            continue
        for off, ln in es_to_file(runs, s0 + 4, 1):
            view[off] = QUANT_BYTE
        tb = 0
        for off, ln in es_to_file(runs, s0 + 5, TAG_BYTES):
            for j in range(ln):
                kind[off + j] = 2
                tags.append((off + j, k, tb))
                tb += 1
        for off, ln in es_to_file(runs, s0 + 5 + TAG_BYTES, end - (s0 + 5 + TAG_BYTES)):
            kind[off:off + ln] = 1
    if untagged:
        raise SystemExit(f'{untagged} pictures too small for a tag (not handled)')
    tag_at = {off: (k, tb) for off, k, tb in tags}
    # Run-length segments.
    segs, stored = [], bytearray()
    change = np.flatnonzero(np.diff(kind)) + 1
    bounds = [0, *change.tolist(), n]
    for a, b in zip(bounds[:-1], bounds[1:]):
        kd = int(kind[a])
        if kd == 0:
            segs.append((a, b - a, 0, 0, len(stored)))
            stored += view[a:b]
        elif kd == 1:
            segs.append((a, b - a, 1, 0, 0))
        else:  # split where the picture or the tag's byte order changes
            s = a
            while s < b:
                k, tb = tag_at[s]
                e = s + 1
                while e < b and tag_at[e] == (k, tb + (e - s)):
                    e += 1
                segs.append((s, e - s, 2, k, tb))
                s = e
    hdr = 96
    seg_off = hdr
    stored_off = seg_off + 32 * len(segs)
    pic_off = stored_off + len(stored)
    frame_tab = pic_off + 4 * len(pics)
    data_off = frame_tab + 16 * len(frames)
    out = bytearray()
    out += MAGIC + struct.pack('<IIQQQQIIQIIIIQQ', 1, len(segs), n, seg_off, stored_off, len(stored), len(pics), 0,
                               pic_off, width, height, len(frames), int(round(fps * 1000)), frame_tab, 0)
    assert len(out) == hdr, len(out)
    for s in segs:
        out += struct.pack('<QQIIQ', *s)
    out += stored
    out += b''.join(struct.pack('<I', disp) for _, _, disp in pics)
    pos = data_off
    for f, key in frames:
        out += struct.pack('<QII', pos, len(f), 1 if key else 0)
        pos += len(f)
    for f, _ in frames:
        out += f
    return bytes(out), segs


def parse_hevc(blob):
    if blob[:8] != MAGIC:
        raise SystemExit('not a .hevc movie')
    (ver, nseg, size, seg_off, stored_off, stored_len, npic, _, pic_off, w, h, nfr, fps, frame_tab,
     _) = struct.unpack('<IIQQQQIIQIIIIQQ', blob[8:96])
    segs = [struct.unpack('<QQIIQ', blob[seg_off + 32 * i:seg_off + 32 * i + 32]) for i in range(nseg)]
    pics = struct.unpack(f'<{npic}I', blob[pic_off:pic_off + 4 * npic])
    frames = []
    for i in range(nfr):
        o, sz, fl = struct.unpack('<QII', blob[frame_tab + 16 * i:frame_tab + 16 * i + 16])
        frames.append((blob[o:o + sz], bool(fl & 1)))
    return dict(size=size, segs=segs, stored=blob[stored_off:stored_off + stored_len], pics=pics, w=w, h=h,
                frames=frames, fps=fps / 1000)


def synthesize(o, movie_id=1):
    out = bytearray(o['size'])
    for off, ln, kd, pic, src in o['segs']:
        if kd == 0:
            out[off:off + ln] = o['stored'][src:src + ln]
        elif kd == 2:
            out[off:off + ln] = make_tag(movie_id, o['pics'][pic])[src:src + ln]
    return bytes(out)


def yuv_frames(ffmpeg, path, w, h, fmt=None):
    """The decoded pictures one at a time (I420 bytes), streamed: a long movie does not fit in memory whole."""
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error']
    if fmt:
        cmd += ['-f', fmt]
    cmd += ['-i', path, '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    fs = w * h * 3 // 2
    try:
        while True:
            b = p.stdout.read(fs)
            if len(b) < fs:
                break
            yield b
    finally:
        p.stdout.close()
        p.kill()
        p.wait()


def psnr(a, b):
    e = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if e == 0 else 10 * math.log10(255.0 ** 2 / e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--fraction', type=float, default=0.35)
    ap.add_argument('--gop', type=int, default=120)
    ap.add_argument('--encoder', default='libx265', choices=['libx265', 'hevc_nvenc'])
    ap.add_argument('--ffmpeg', default='C:/ffmpeg/ffmpeg.exe')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--psnr-every', type=int, default=30)
    a = ap.parse_args()
    d = open(a.src, 'rb').read()
    es, runs = demux(d)
    pics, gops = pictures(es)
    if a.check:
        return check(a, d, es, runs, pics)
    width, height, fps = probe(es)
    video_bytes = sum(ln for _, _, ln in runs)
    seconds = len(pics) / fps
    bitrate = int(video_bytes * 8 / seconds * a.fraction)
    with tempfile.TemporaryDirectory() as tmp:
        m1v = os.path.join(tmp, 'v.m1v')
        open(m1v, 'wb').write(es)
        h265 = os.path.join(tmp, 'v.hevc')
        encode(a.ffmpeg, m1v, h265, a.encoder, bitrate, a.gop, fps)
        frames = access_units(open(h265, 'rb').read())
    if len(frames) != len(pics):
        raise SystemExit(f'{len(frames)} H.265 frames for {len(pics)} pictures')
    if not frames[0][1]:
        raise SystemExit('the first H.265 frame is not a keyframe')
    blob, segs = build(d, runs, pics, frames, width, height, fps)
    tmp = a.dst + '.part'
    open(tmp, 'wb').write(blob)
    os.replace(tmp, a.dst)
    h265_bytes = sum(len(f) for f, _ in frames)
    print(f'{os.path.basename(a.src)}: {width}x{height} {fps:.2f} fps, {len(pics)} pictures ({gops} GOPs), MPEG-1 '
          f'{video_bytes / 1e6:.1f} MB at {video_bytes * 8 / seconds / 1e6:.2f} Mbit/s -> H.265 {h265_bytes / 1e6:.1f} MB '
          f'at {h265_bytes * 8 / seconds / 1e6:.2f} Mbit/s ({a.encoder}); {len(d) / 1e6:.1f} MB -> {len(blob) / 1e6:.1f} '
          f'MB, {len(segs)} segments', flush=True)


def check(a, d, es, runs, pics):
    o = parse_hevc(open(a.dst, 'rb').read())
    view = synthesize(o, movie_id=77)
    if len(view) != len(d):
        raise SystemExit('the size differs')  # (the USM's)
    # Every byte outside the slice data as in the USM; the slice data as the format says.
    slice_mask = np.zeros(len(d), bool)
    for s0, end, _ in pics:
        for off, ln in es_to_file(runs, s0 + 4, end - (s0 + 4)):
            slice_mask[off:off + ln] = True
    dv, vv = np.frombuffer(d, np.uint8), np.frombuffer(view, np.uint8)
    outside = np.flatnonzero((dv != vv) & ~slice_mask)
    print(f'bytes outside the slice data that differ: {len(outside)}')
    # The tags, read back from the view's ES as the hook reads them (after the slice start code and its header byte).
    es_v, _ = demux(view)
    bad = 0
    for k, (s0, end, disp) in enumerate(pics):
        t = es_v[s0 + 5:s0 + 5 + TAG_BYTES]
        if es_v[s0:s0 + 3] != b'\x00\x00\x01' or es_v[s0 + 4] != QUANT_BYTE or t != make_tag(77, disp):
            bad += 1
    print(f'pictures: {len(pics)}, tags not where they should be: {bad}; the ES zero after the tags: '
          f'{all(not any(es_v[s0 + 5 + TAG_BYTES:end]) for s0, end, _ in pics)}')
    # No start code where there was none: every 00 00 01 in the view's ES is one of the SFD's.
    def codes(b):
        out, p = [], b.find(b'\x00\x00\x01')
        while p >= 0:
            out.append(p)
            p = b.find(b'\x00\x00\x01', p + 3)
        return out
    print(f'start codes: USM {len(codes(es))}, as shown ' + str(len(codes(es_v))) + ' (the slices after the first in '
          'each picture are gone)')
    w, h = o['w'], o['h']
    with tempfile.TemporaryDirectory() as tmp:
        hv = os.path.join(tmp, 'v.hevc')
        open(hv, 'wb').write(b''.join(f for f, _ in o['frames']))
        vals, n_ref, n_got = [], 0, 0
        m1v = os.path.join(tmp, 'v.m1v')
        open(m1v, 'wb').write(es)
        ref_it, got_it = yuv_frames(a.ffmpeg, m1v, w, h, 'mpegvideo'), yuv_frames(a.ffmpeg, hv, w, h, 'hevc')
        for r, g in zip(ref_it, got_it):
            if n_ref % a.psnr_every == 0:
                vals.append(psnr(np.frombuffer(r, np.uint8), np.frombuffer(g, np.uint8)))
            n_ref += 1
        n_got = n_ref + sum(1 for _ in got_it)
        n_ref += sum(1 for _ in ref_it)
    print(f'frames: MPEG-1 {n_ref}, H.265 {n_got}, keyframes {sum(1 for _, k in o["frames"] if k)}')
    print(f'PSNR every {a.psnr_every}th frame: mean {np.mean(vals):.1f} dB, min {np.min(vals):.1f} dB')


if __name__ == '__main__':
    main()
