# Emulator screen tests (x4prosim)

CI job `emu` (`.github/workflows/ci.yml`) boots the `x4-pro-debug` build in
[x4prosim](https://github.com/prokrypt/x4prosim) (QEMU model of the X4 Pro), walks
`steps.txt` through the serial remote (`docs/serial-remote.md`) and compares each
`shot NAME` with `goldens/NAME.png`, pixel for pixel outside the boxes in
`goldens/NAME.mask` (`X,Y,W,H` per line). It also fails on a crash or `[ERR]` in the
firmware log and on any DC-unbalanced LUT the emulated panel sees. The job uploads
`emu-x4-pro-debug-out` every run: firmware log, shots, `diff-NAME.png` (red = differs,
blue = masked), `guest.log`.

A shot with no golden is reported as NEW and does not fail. When a PR changes a screen on
purpose, take its shots from that artifact (`run/NAME.png`), copy them over `goldens/`
and commit them in the same PR; the diff pictures show what moved.

Valid in the emulator: layout and final pixels of B/W and register-LUT (gray, DU)
content. Not valid: OTP Half/Full waveform timing, power numbers, light sleep.

Local run (QEMU built in an x4prosim checkout, `pip install esptool pillow`,
`apt install dosfstools mtools`):

    pio run -e x4-pro-debug
    test/emu/run.sh .pio/build/x4-pro-debug ../x4prosim /tmp/emu-out
