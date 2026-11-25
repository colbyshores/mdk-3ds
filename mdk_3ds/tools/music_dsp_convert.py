#!/usr/bin/env python3
# ============================================================================
# music_dsp_convert.py -- the 3DS's music as DSP-ADPCM, from a soundtrack tree.
#
# USAGE:  music_dsp_convert.py <soundtrack-dir> <original-dir> <out-dir>
#
#   <soundtrack-dir>  a drop-in SNI soundtrack (the vini1264 high-quality mod:
#                     TRAVERSE/LEVELn/LEVELnO.SNI and LEVELnS.SNI with the GOG
#                     entry names). A level the soundtrack does not replace is
#                     read from <original-dir>.
#   <original-dir>    the GOG data (MDK/): the loudness reference.
#   <out-dir>         mdk_3ds/music: levelN/<name>.dsp and menu/<name>.dsp (the
#                     front end's MAINSONG and OPTSONG), lower case, committed.
#
# Run it with `python3 -I` (the soundtrack is downloaded data). It needs ffmpeg
# (with soxr), numpy and a C compiler (for the vendored encoder, dspadpcm/).
#
# WHICH ENTRIES. A level's music is what LevelMusic plays: the CMI's
# arena-to-track table, or CORRIDOR (LEVELnS.SNI) for an arena without one. So
# an entry is converted when it carries the SNI music flag (2) AND its name is
# spelled in the level's CMI, or it is CORRIDOR. Music-flagged entries no CMI
# names (level 4's SONG_ACTIVE/MEDIUM/LIGHT, AMB1-3, GOLD) are never played and
# are skipped. A name the CMI spells that the GOG archives lack (the
# soundtrack's AMB_DRIP and H9WATER on level 3, TRACK12 on level 7: their
# arenas are silent with the GOG data) is converted: the game asks for it.
#
# THE SIGNAL. Each entry's WAV is decoded by ffmpeg, mixed to mono (the mean of
# the channels) and resampled to 32000 Hz with soxr at 28 bits of precision.
# The level's soundtrack is normalised to the ORIGINAL's loudness, track by
# track: the gain is the original entry's EBU R128 integrated loudness less the
# soundtrack entry's, both measured on the same mono 32 kHz signal. A track the
# original lacks takes the median gain of the soundtrack's replaced tracks. A
# gain that would clip goes through a lookahead limiter (ffmpeg alimiter) and
# is corrected until the loudness is the original's again (reported). An entry
# whose bytes equal the original's gets no gain. Then PCM16 (rounded) and the
# vendored gc-dspadpcm-encode.
#
# LOOPS. The game loops every music entry whole (SNI flag 1; the WAV's cue
# chunks are not read by the engine), and so does the 3DS player: the .dsp's
# own loop fields are not used.
#
# A report of every track (source format, seconds, loudness, gain, peak, bytes)
# is written to <out-dir>/CONVERT_REPORT.txt.
# ============================================================================
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 32000
LEVELS = (3, 4, 5, 6, 7, 8)
FLAG_MUSIC = 2


def find_ci(root, *parts):
    """A path under root, matching each part case-insensitively."""
    cur = root
    for part in parts:
        if not os.path.isdir(cur):
            return None
        hit = None
        for name in os.listdir(cur):
            if name.lower() == part.lower():
                hit = name
                break
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def sni_entries(path):
    """{NAME: (flags, wav bytes)} of a retail SNI (directory at 0x14)."""
    data = open(path, 'rb').read()
    count = struct.unpack_from('<I', data, 0x14)[0]
    out = {}
    at = 0x18
    for _ in range(count):
        name = data[at:at + 12].split(b'\0')[0].decode('latin1').upper()
        flags, _vol, offset, length = struct.unpack_from('<HHII', data, at + 12)
        at += 24
        if length == 0xFFFFFFFF:
            continue
        body = data[offset + 4:offset + 4 + length]
        if body[:4] == b'RIFF':
            out[name] = (flags, body)
    return out


def level_entries(root, level):
    """The level's music-flagged WAV entries: LEVELnO.SNI, then LEVELnS.SNI."""
    found = {}
    for suffix in ('S', 'O'):
        path = find_ci(root, 'TRAVERSE', 'LEVEL%d' % level, 'LEVEL%d%s.SNI' % (level, suffix))
        if path is None:
            continue
        for name, (flags, body) in sni_entries(path).items():
            if flags & FLAG_MUSIC:
                found[name] = body
    return found


def decode_mono(wav_bytes, work):
    """The entry as float32 mono at RATE (soxr, precision 28)."""
    src = os.path.join(work, 'in.wav')
    with open(src, 'wb') as f:
        f.write(wav_bytes)
    raw = subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', src, '-af',
         'aresample=resampler=soxr:precision=28,pan=mono|c0=0.5*c0+0.5*c1'
         if channels_of(wav_bytes) == 2 else 'aresample=resampler=soxr:precision=28',
         '-ar', str(RATE), '-ac', '1', '-f', 'f32le', '-'],
        check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype='<f4').astype(np.float64)


def channels_of(wav_bytes):
    at = 12
    while at + 8 <= len(wav_bytes):
        cid = wav_bytes[at:at + 4]
        size = struct.unpack_from('<I', wav_bytes, at + 4)[0]
        if cid == b'fmt ':
            return struct.unpack_from('<H', wav_bytes, at + 10)[0]
        at += 8 + size + (size & 1)
    return 1


def describe(wav_bytes):
    at = 12
    fmt = None
    data = 0
    while at + 8 <= len(wav_bytes):
        cid = wav_bytes[at:at + 4]
        size = struct.unpack_from('<I', wav_bytes, at + 4)[0]
        if cid == b'fmt ':
            _f, ch, rate, _br, _ba, bits = struct.unpack_from('<HHIIHH', wav_bytes, at + 8)
            fmt = (ch, rate, bits)
        elif cid == b'data':
            data = size
        at += 8 + size + (size & 1)
    ch, rate, bits = fmt
    return '%dch %dHz %db %.2fs' % (ch, rate, bits, data / (ch * bits // 8) / rate)


def loudness(samples, work):
    """EBU R128 integrated loudness (LUFS) of a mono signal at RATE."""
    path = os.path.join(work, 'loud.f32')
    samples.astype('<f4').tofile(path)
    err = subprocess.run(
        ['ffmpeg', '-v', 'info', '-nostats', '-f', 'f32le', '-ar', str(RATE), '-ac', '1',
         '-i', path, '-af', 'ebur128=framelog=quiet', '-f', 'null', '-'],
        check=True, capture_output=True, text=True).stderr
    hits = re.findall(r'I:\s+(-?[\d.]+|-inf) LUFS', err)
    return float(hits[-1]) if hits and hits[-1] != '-inf' else None


def bni_wav(path, wanted):
    """The RIFF WAV entry `wanted` of a BNI archive (u32 size, u32 count, then
    12-byte names with u32 offsets from 4), or None."""
    data = open(path, 'rb').read()
    count = struct.unpack_from('<I', data, 4)[0]
    at = 8
    for _ in range(count):
        name = data[at:at + 12].split(b'\0')[0].decode('latin1').upper()
        offset = struct.unpack_from('<I', data, at + 12)[0] + 4
        at += 16
        if name == wanted and data[offset:offset + 4] == b'RIFF':
            size = struct.unpack_from('<I', data, offset + 4)[0]
            return data[offset:offset + 8 + size]
    return None


def menu_entries(root):
    """{MAINSONG, OPTSONG: wav bytes} of a tree, where it has them."""
    found = {}
    bni = find_ci(root, 'MISC', 'OPTIONS.BNI')
    if bni is not None:
        wav = bni_wav(bni, 'MAINSONG')
        if wav is not None:
            found['MAINSONG'] = wav
    sni = find_ci(root, 'MISC', 'MDKSOUND.SNI')
    if sni is not None:
        entries = sni_entries(sni)
        if 'OPTSONG' in entries:
            found['OPTSONG'] = entries['OPTSONG'][1]
    return found


def cmi_names(root, level):
    path = find_ci(root, 'TRAVERSE', 'LEVEL%d' % level, 'LEVEL%d.CMI' % level)
    return open(path, 'rb').read() if path else b''


kCeiling = 32767.0 / 32768.0


def limit(sig, gain_db, work):
    """`sig` with `gain_db` through a lookahead limiter, peaks under kCeiling."""
    src = os.path.join(work, 'lim.f32')
    sig.astype('<f4').tofile(src)
    raw = subprocess.run(
        ['ffmpeg', '-v', 'error', '-f', 'f32le', '-ar', str(RATE), '-ac', '1', '-i', src, '-af',
         'volume=%.4fdB,alimiter=limit=0.95:attack=5:release=50:level=0:latency=1' % gain_db,
         '-f', 'f32le', '-'], check=True, capture_output=True).stdout
    out = np.frombuffer(raw, dtype='<f4').astype(np.float64)
    out = out[:sig.size]
    if out.size < sig.size:
        out = np.concatenate([out, np.zeros(sig.size - out.size)])
    return np.clip(out, -kCeiling, kCeiling)


def build_encoder(work):
    exe = os.path.join(work, 'dspadpcm')
    src = os.path.join(HERE, 'dspadpcm')
    subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-w', '-o', exe,
                    os.path.join(src, 'main.c'), os.path.join(src, 'grok.c'), '-lm'],
                   check=True)
    return exe


def write_wav16(path, pcm):
    data = pcm.astype('<i2').tobytes()
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def main():
    if len(sys.argv) != 4:
        sys.stderr.write('usage: music_dsp_convert.py <soundtrack-dir> <original-dir> <out-dir>\n')
        return 2
    mod_root, orig_root, out_root = sys.argv[1:4]
    work = tempfile.mkdtemp(prefix='mdk-music-')
    try:
        encoder = build_encoder(work)
        plan = []  # (level, name, mod bytes, orig bytes or None)
        for level in LEVELS:
            orig = level_entries(orig_root, level)
            # The soundtrack's entries over the original's: a level (or an
            # archive -- most levels' LEVELnS.SNI, CORRIDOR's) it does not
            # replace keeps the original's.
            mod = dict(orig)
            mod.update(level_entries(mod_root, level))
            cmi = cmi_names(orig_root, level)
            for name in sorted(mod):
                if name != 'CORRIDOR' and name.encode('latin1') not in cmi:
                    continue
                plan.append(('level%d' % level, name, mod[name], orig.get(name)))
        # THE FRONT END'S TWO SONGS (the main menu's MAINSONG in MISC/OPTIONS.BNI,
        # the options' OPTSONG in MISC/MDKSOUND.SNI): what the original plays on
        # the music bus before a level (C# MainMenu, MenuItems). The soundtrack
        # replaces MAINSONG; OPTSONG is the original's.
        menu_orig = menu_entries(orig_root)
        menu_mod = dict(menu_orig)
        menu_mod.update(menu_entries(mod_root))
        for name in sorted(menu_mod):
            plan.append(('menu', name, menu_mod[name], menu_orig.get(name)))
        # Pass 1: the gains of the replaced tracks.
        gains = {}
        signals = {}
        for level, name, mod_bytes, orig_bytes in plan:
            sig = decode_mono(mod_bytes, work)
            signals[(level, name)] = sig
            if orig_bytes is not None and orig_bytes != mod_bytes:
                lm = loudness(sig, work)
                lo = loudness(decode_mono(orig_bytes, work), work)
                gains[(level, name)] = (lo - lm, lo, lm)
        # The levels' replaced tracks only (the menu's MAINSONG does not move it).
        replaced = sorted(g[0] for k, g in gains.items() if k[0] != 'menu')
        median = replaced[len(replaced) // 2] if replaced else 0.0
        # Pass 2: gain, peak, PCM16, encode.
        report = ['# music_dsp_convert.py report: level name | source | loudness orig/mod '
                  '(LUFS) | gain dB | peak after gain | .dsp bytes',
                  '# median gain of the replaced tracks: %+.2f dB' % median]
        for level, name, mod_bytes, orig_bytes in plan:
            sig = signals[(level, name)]
            lo = None
            if orig_bytes == mod_bytes:
                gain_db, note = 0.0, 'original'
            elif (level, name) in gains:
                gain_db, lo, lm = gains[(level, name)]
                note = 'replaced %.1f/%.1f' % (lo, lm)
            else:
                gain_db, note = median, 'new (median gain)'
            scaled = sig * (10.0 ** (gain_db / 20.0))
            peak = float(np.max(np.abs(scaled))) if scaled.size else 0.0
            if peak > kCeiling and orig_bytes == mod_bytes:
                # An unchanged original over full scale after the resampler (an
                # 8-bit original at full scale rings a little): clipped, the
                # few samples the resampler pushed past the ceiling, as the
                # game's own 16-bit output would clip them. No limiter on it.
                scaled = np.clip(scaled, -kCeiling, kCeiling)
                note += ', %d samples clipped' % int(np.sum(np.abs(sig) > kCeiling))
                peak = kCeiling
            elif peak > kCeiling:
                # The original is louder than the soundtrack can be made by gain
                # alone (an 8-bit original is squashed flat): a lookahead limiter
                # (ffmpeg alimiter, 5 ms attack, 50 ms release, delay
                # compensated) holds the peaks under the ceiling, and the gain
                # is corrected until the loudness is the original's again.
                target = lo if (level, name) in gains else None
                for _ in range(4):
                    scaled = limit(sig, gain_db, work)
                    if target is None:
                        break
                    now = loudness(scaled, work)
                    if now is None or abs(target - now) < 0.1:
                        break
                    gain_db += target - now
                peak = float(np.max(np.abs(scaled)))
                note += ', limited (%.1f LUFS)' % (loudness(scaled, work) or 0.0)
            pcm = np.clip(np.round(scaled * 32768.0), -32768, 32767)
            folder = os.path.join(out_root, level)
            os.makedirs(folder, exist_ok=True)
            wav = os.path.join(work, 'enc.wav')
            write_wav16(wav, pcm)
            out = os.path.join(folder, name.lower() + '.dsp')
            subprocess.run([encoder, wav, out], check=True, capture_output=True)
            if os.path.exists(out + '.wav'):
                os.remove(out + '.wav')
            size = os.path.getsize(out)
            sha = hashlib.sha256(open(out, 'rb').read()).hexdigest()[:16]
            report.append('%-6s %-12s | %s | %s | %+.2f | %.3f | %d %s' %
                          (level, name, describe(mod_bytes), note, gain_db, peak, size, sha))
            print(report[-1], flush=True)
        with open(os.path.join(out_root, 'CONVERT_REPORT.txt'), 'w') as f:
            f.write('\n'.join(report) + '\n')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
