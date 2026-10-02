#!/bin/sh
# Usage: SDFAT=/path/to/SdFat/src ./run.sh
set -e
S=${SDFAT:?set SDFAT to SdFat src dir}
cd "$(dirname "$0")"
g++ -std=c++17 -w -include shim.h -DSPI_DRIVER_SELECT=3 -DENABLE_ARDUINO_FEATURES=0 -DENABLE_ARDUINO_SERIAL=0 -DENABLE_ARDUINO_STRING=0 -DSDFAT_FILE_TYPE=3 -DUSE_BLOCK_DEVICE_INTERFACE=1 -I"$S" sd_mtime_check.cpp $(ls "$S"/FatLib/*.cpp "$S"/ExFatLib/*.cpp "$S"/FsLib/*.cpp "$S"/common/*.cpp 2>/dev/null) -o /tmp/sd_mtime_check
/tmp/sd_mtime_check
