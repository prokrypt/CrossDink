"""Embed generated outline files only for opted-in S3 environments.

Each TTF is stored as raw DEFLATE (PackedAsset) and inflated into PSRAM when
its reader family is first used, which roughly halves its flash footprint.
OutlineFingerprint still hashes the uncompressed files, so font cache IDs only
change when the fonts themselves change."""
from pathlib import Path
import hashlib
import zlib


def _deflate_raw(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return compressor.compress(data) + compressor.flush()


def generate(root):
    source = root / 'lib/EpdFont/scalableFonts'
    output = root / 'lib/ScalableFont/ScalableAssets.generated.h'
    lines = ['#pragma once', '#include <cstdint>']
    identity = hashlib.sha256()
    for filename in (source/'manifest.txt').read_text().splitlines():
        if not filename.strip():
            continue
        data = (source/filename).read_bytes()
        identity.update(filename.encode()); identity.update(data)
        name = Path(filename).stem
        packed = _deflate_raw(data)
        lines.append(f'// {filename}: {len(data)} bytes raw, {len(packed)} packed')
        lines.append(f'static constexpr uint32_t {name}RawSize = {len(data)}u;')
        lines.append(f'alignas(4) static const uint8_t {name}Packed[] = {{')
        lines.extend(','.join(f'0x{b:02x}' for b in packed[i:i+24])+',' for i in range(0,len(packed),24))
        lines.append('};')
    lines.append(f'static constexpr uint32_t OutlineFingerprint = 0x{identity.hexdigest()[:8]}u;')
    content='\n'.join(lines)+'\n'
    if not output.exists() or output.read_text()!=content: output.write_text(content)

try:
    Import('env')
except NameError:
    generate(Path(__file__).resolve().parents[1])
else:
    flags = env.GetProjectOption('build_flags', [])
    if 'CROSSDINK_SCALABLE_FONTS=1' in ' '.join(flags):
        generate(Path(env.subst('$PROJECT_DIR')))
