---
title: EPUB Indexing
nav_order: 8.5
---

# EPUB Indexing

Before CrossDink can display an EPUB chapter, it lays out the chapter into
pages and saves that layout in the book's cache.

CrossDink uses one method, IncreMENTAL: it builds enough pages to show your
current position, then keeps building the rest of the chapter in small
background steps while you read, until the chapter cache is complete. Think of
it like loading the first screen of a long web page first, then preparing the
rest while you read. The background work yields to page turns and other input.

CrossDink saves the pages it has already built, so leaving the book does not
discard that readable progress, and reopening a partly built chapter resumes
the background build right away.

Expect a visible **Indexing** popup if you jump far ahead, follow a link to an
unbuilt part of the chapter, or turn pages faster than the background work can
stay ahead. That is normal: CrossDink is building just enough additional pages
to make the requested position readable.

KOReader Sync uses the same content location rather than the other device's
page number. If a synced location is beyond this device's built pages,
CrossDink indexes forward until that location is available.

## If A Book Is Still Slow Or Cannot Index

Indexing does not simplify the publisher's CSS, images, or tables. If a
difficult EPUB is still slow or runs out of memory, try a lighter
[EPUB Render Mode](./epub-render-modes.md) or
[optimize](https://inky.crossink.dev) the EPUB before copying it to the device.
