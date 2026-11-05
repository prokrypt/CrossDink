"""Strip the embedded reader TTFs down to the tables FreeInkFont reads.

The firmware only looks up GSUB liga/rlig (lib/ScalableFont/ScalableGsub.h) and
GPOS kern (freeink-sdk/libs/font/FreeInkFont/src/Gpos.cpp). Every other layout
feature (small caps, old-style figures, mark positioning, ...) is dead weight in
flash. Run after replacing a font in lib/EpdFont/scalableFonts; safe to re-run.

Requires fonttools (pip install fonttools).
"""
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

FEATURES = ['liga', 'rlig', 'kern']


def main():
    source = Path(__file__).resolve().parents[1] / 'lib/EpdFont/scalableFonts'
    options = subset.Options()
    options.layout_features = FEATURES
    options.name_IDs = ['*']  # keep copyright and license strings
    options.name_languages = ['*']
    options.hinting = False
    options.drop_tables += ['DSIG', 'STAT']
    options.notdef_outline = True
    for filename in (source / 'manifest.txt').read_text().splitlines():
        path = source / filename
        before = path.stat().st_size
        font = TTFont(path)
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=font.getBestCmap().keys())
        subsetter.subset(font)
        font.save(path)
        print(f'{filename:28} {before:>7} B -> {path.stat().st_size:>7} B')


if __name__ == '__main__':
    main()
