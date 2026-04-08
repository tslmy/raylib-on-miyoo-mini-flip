#!/bin/sh
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="/mnt/SDCARD/.tmp_update/lib/parasyte:/customer/lib:/mnt/SDCARD/miyoo/lib:$LD_LIBRARY_PATH"
./raylib-cube > raylib-cube.log 2>&1
