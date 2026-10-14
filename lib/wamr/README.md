# Vendored WAMR

wasm-micro-runtime 2.4.0, from the ESP component registry archive
`espressif/wasm-micro-runtime` 2.4.0~1, cut to what the classic interpreter needs
on ESP-IDF: `core/iwasm/{include,common,interpreter}` (plus the two AOT/compilation
headers `wasm_memory.c` includes unconditionally), `core/shared/{utils,mem-alloc}`,
and the `esp-idf` platform. Unmodified except `invokeNative_xtensa.s` renamed to `.S`, so it goes
through gcc (the bare assembler rejects the build's `-mlongcalls`); build options live in `library.json`.

Only `x4-pro-debug` links it (`lib_ignore = wamr` everywhere else). Used by
`src/activities/goodies/GoodyModuleActivity.cpp`; modules are built from `goodies/`.

License: Apache-2.0 WITH LLVM-exception (`LICENSE`, `ATTRIBUTIONS.md`).
