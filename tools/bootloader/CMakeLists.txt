# Second-stage bootloader for the X4 Pro, built by CI (bootloader job) with ESP-IDF, not by PlatformIO:
# pioarduino ships Arduino's prebuilt bootloaders and ignores the bootloader options in platformio.ini.
# The app is a dummy; only build/bootloader/bootloader.bin is used (Goodies > Flash bootloader).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(crossdink_bootloader)
