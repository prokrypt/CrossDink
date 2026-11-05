# Scalable fonts on ESP32-S3 devices

CrossDink includes Bitter and Lexend Deca as scalable TrueType fonts on every
supported reader. You can also add your own static `.ttf` or `.otf` fonts, and `.cpfont`
font packs still work alongside them.

## Add your own scalable fonts

1. Copy your `.ttf` or `.otf` files into a family folder on the SD card, such as
   `/fonts/My Font/`. The hidden `/.fonts/My Font/` location works too.
2. If you copied the files using a computer, restart the reader or refresh its
   font list.
3. Choose the family from **Settings > Reader > Font Options > Font Family**, or
   from the font picker while reading.

You can also upload `.ttf` or `.otf` files through the **Fonts** tab in the reader's web
interface. See [SD Card Fonts](./sd-card-fonts.md) for how to open that interface.
No font conversion or separate files for each size are needed.

For the full range of text styles, add all four files for a family: **regular**,
**bold**, **italic**, and **bold italic**. CrossDink can use a family with fewer
styles, but missing styles may appear in another available style. If the files
are in a family folder, that folder's name appears in the font picker. Keep
scalable fonts and `.cpfont` files in separate family folders.

## Adjust the appearance

After selecting a custom TTF family, open **TTF Rendering** under **Settings >
Reader > Font Options**, or from the in-book font menu. These controls let you
adjust how the letters look on the e-ink screen. Changes to a family apply to
all its styles and are remembered for that family. This option appears only
for custom TTF fonts; built-in and `.cpfont` fonts do not use it.

Custom TTF fonts use the existing **8–22 pt** size choices. Changing fonts or
their appearance may make CrossDink lay out the current book again, so page
positions can change. Your bookmarks and clippings remain saved.

## Publisher fonts in EPUBs

Many EPUBs carry their own fonts (`@font-face` rules pointing at `.ttf` or
`.otf` files inside the book). **Publisher Fonts** under **Settings > Reader >
Font Options**, or the **Font** tab of the in-book menu, picks how they are
used:

- **Off**: all text uses your reader font.
- **Ignore Body Font** (default): headings, decorative and special text use the
  book's fonts; the body text keeps your reader font.
- **All Text**: the book's fonts are used everywhere they are assigned.

Publisher fonts need **Embedded Style** on and are not used in Light render
mode. A face loads only when a page uses it (up to 2 MiB per face, 4 MiB per
book, 32 faces). It is copied into the book's cache on first use, so later
opens skip that step. A book without embedded fonts lays out exactly as with
the setting off. Changing the setting lays out the current chapter again.

To drop a book's fonts before it reaches the device, turn on **Remove Embedded
Fonts** in the web portal's upload settings (off by default).

## Supported files and limits

- Use static TrueType `.ttf` or OpenType CFF `.otf` files. Some CFF fonts have a
  `.ttf` filename; CrossDink checks the file contents. Variable fonts and font
  collections are not supported.
- Each file must be **2 MiB or smaller**, and the four files in one family must
  total **6 MiB or less**.
- If both `/fonts` and `/.fonts` contain the same family, the copy in
  `/.fonts` takes precedence.

If a font does not appear, check its file type and size, then refresh the font
list or restart the reader. If the reader has too little free memory to open a
font, it may use a fallback font instead.
