#!/usr/bin/env python3
"""Generate cover-art / tag test assets.

Every container stores pictures differently, so the coverage check needs one
file per layout:

  t_cover.wav       ID3v2 APIC in a trailing "id3 " chunk (mutagen default,
                    what most taggers append after the audio payload)
  t_cover_pre.wav   the same tag moved in front of "data"
  t_list.wav        RIFF LIST/INFO text tags (INAM/IART/IPRD)
  t_cover.flac      FLAC METADATA_BLOCK_PICTURE
  t_cover.ogg       Vorbis METADATA_BLOCK_PICTURE base64 comment
  t_cover.opus      Opus METADATA_BLOCK_PICTURE base64 comment
  t_legacy.opus     legacy COVERART + COVERARTMIME comments
  t_badpic.ogg      malformed base64 / non-image COVERART (must be skipped)

Requires `mutagen` (pip install mutagen) and, only when the cover images are
missing, ffmpeg.  Run inside WSL/Linux:

    python3 tests/gen_cover_assets.py [ffmpeg] tests/assets

Pair it with tests/cover_check.sh.
"""

import base64
import os
import shutil
import struct
import subprocess
import sys

from mutagen.flac import FLAC, Picture
from mutagen.id3 import APIC, TIT2, TPE1
from mutagen.oggopus import OggOpus
from mutagen.oggvorbis import OggVorbis
from mutagen.wave import WAVE


def parse_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("%s is not RIFF/WAVE" % path)
    chunks = []
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        chunks.append((cid, data[pos + 8:pos + 8 + size]))
        pos += 8 + size + (size & 1)
    return chunks


def write_wav(path, chunks):
    body = bytearray()
    for cid, payload in chunks:
        body += cid + struct.pack("<I", len(payload)) + payload
        if len(payload) & 1:
            body += b"\x00"
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body) + 4) + b"WAVE" + body)


def info_chunk(title, artist, album):
    """RIFF LIST/INFO chunk carrying the three text tags we parse."""
    body = bytearray(b"INFO")
    for cid, text in ((b"INAM", title), (b"IART", artist), (b"IPRD", album)):
        value = text.encode("utf-8") + b"\x00"
        body += cid + struct.pack("<I", len(value)) + value
        if len(value) & 1:
            body += b"\x00"
    return b"LIST", bytes(body)


def insert_before_data(chunks, extra):
    """Return chunks with `extra` (list of chunks) placed before "data"."""
    out = []
    for cid, payload in chunks:
        if cid == b"data":
            out.extend(extra)
        out.append((cid, payload))
    return out


def ensure_covers(out, ffmpeg):
    jpg = os.path.join(out, "cover.jpg")
    png = os.path.join(out, "cover.png")
    if os.path.exists(jpg) and os.path.exists(png):
        return jpg, png

    def run(size, dst):
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "color=c=red:s=%dx%d" % (size, size),
                        "-frames:v", "1", dst], check=True)

    if not os.path.exists(jpg):
        run(64, jpg)
    if not os.path.exists(png):
        run(32, png)
    return jpg, png


def main():
    ffmpeg = sys.argv[1] if len(sys.argv) > 1 else "ffmpeg"
    assets = sys.argv[2] if len(sys.argv) > 2 else "tests/assets"
    out = os.path.join(assets, "covers")
    os.makedirs(out, exist_ok=True)

    jpg, png = ensure_covers(out, ffmpeg)
    jpeg = open(jpg, "rb").read()
    png_data = open(png, "rb").read()

    # ---- WAV: ID3v2 text + APIC, appended after "data" by mutagen ----
    wav_dst = os.path.join(out, "t_cover.wav")
    shutil.copy(os.path.join(assets, "t.wav"), wav_dst)
    w = WAVE(wav_dst)
    w.add_tags()
    w.tags.add(TIT2(encoding=3, text="Wav Title"))
    w.tags.add(TPE1(encoding=3, text="Wav Artist"))
    w.tags.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="Cover", data=jpeg))
    w.save()

    chunks = parse_wav(wav_dst)
    id3 = next((payload for cid, payload in chunks if cid == b"id3 "), None)
    if id3:
        write_wav(os.path.join(out, "t_cover_pre.wav"),
                  insert_before_data([c for c in chunks if c[0] != b"id3 "],
                                     [(b"id3 ", id3)]))

    # ---- WAV: LIST/INFO text tags before "data" ----
    write_wav(os.path.join(out, "t_list.wav"),
              insert_before_data(parse_wav(os.path.join(assets, "t.wav")),
                                 [info_chunk("WAV LIST Title", "WAV LIST Artist",
                                             "WAV LIST Album")]))

    # ---- FLAC: native PICTURE metadata block ----
    flac_dst = os.path.join(out, "t_cover.flac")
    shutil.copy(os.path.join(assets, "t.flac"), flac_dst)
    fl = FLAC(flac_dst)
    pic = Picture()
    pic.type = 3
    pic.mime = "image/jpeg"
    pic.desc = "Cover"
    pic.data = jpeg
    fl.add_picture(pic)
    fl.save()
    block = pic.write()

    # ---- Ogg Vorbis: METADATA_BLOCK_PICTURE base64 comment ----
    ogg_dst = os.path.join(out, "t_cover.ogg")
    shutil.copy(os.path.join(assets, "t.ogg"), ogg_dst)
    vo = OggVorbis(ogg_dst)
    vo["METADATA_BLOCK_PICTURE"] = base64.b64encode(block).decode("ascii")
    vo["TITLE"] = "Ogg Title"
    vo["ARTIST"] = "Ogg Artist"
    vo.save()

    # ---- Opus: METADATA_BLOCK_PICTURE base64 comment ----
    opus_dst = os.path.join(out, "t_cover.opus")
    shutil.copy(os.path.join(assets, "t.opus"), opus_dst)
    op = OggOpus(opus_dst)
    op["METADATA_BLOCK_PICTURE"] = base64.b64encode(block).decode("ascii")
    op["TITLE"] = "Opus Title"
    op["ARTIST"] = "Opus Artist"
    op.save()

    # ---- Opus: legacy COVERART + COVERARTMIME ----
    legacy_dst = os.path.join(out, "t_legacy.opus")
    shutil.copy(os.path.join(assets, "t.opus"), legacy_dst)
    op = OggOpus(legacy_dst)
    op["COVERART"] = base64.b64encode(png_data).decode("ascii")
    op["COVERARTMIME"] = "image/png"
    op.save()

    # ---- Opus: malformed pictures must be skipped, not crash ----
    bad_dst = os.path.join(out, "t_badpic.ogg")
    shutil.copy(os.path.join(assets, "t.ogg"), bad_dst)
    vo = OggVorbis(bad_dst)
    vo["METADATA_BLOCK_PICTURE"] = "!!!not-base64!!!"
    vo["COVERART"] = "AAAA"  # decodes, but is not a known image signature
    vo.save()

    names = ("t_cover.wav", "t_cover_pre.wav", "t_list.wav", "t_cover.flac",
             "t_cover.ogg", "t_cover.opus", "t_legacy.opus", "t_badpic.ogg")
    for name in names:
        path = os.path.join(out, name)
        print("%-16s %8d bytes" % (name, os.path.getsize(path)))
    print("chunk order (t_cover.wav):",
          [cid.decode("latin1") for cid, _ in parse_wav(wav_dst)])


if __name__ == "__main__":
    main()
