#!/bin/sh
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="/mnt/SDCARD/.tmp_update/lib/parasyte:/customer/lib:/mnt/SDCARD/miyoo/lib:$LD_LIBRARY_PATH"

# Set CPU to performance mode for better software rendering
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true

./raylib-cube > raylib-cube.log 2>&1

# Reset CPU governor to ondemand to save battery
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true
