#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Runs on the phone via adb shell stdin. No resident service, root, or heap dumps.
echo "PHONE_UPTIME $(cat /proc/uptime)"
echo "PHONE_TIME $(date '+%Y-%m-%dT%H:%M:%S%z')"
echo "=== SYSTEM_MEMORY_KB ==="
cat /proc/meminfo
echo "=== MEMORY_PRESSURE ==="
cat /proc/pressure/memory 2>&1
grep -E 'oom_kill|allocstall|pswpin|pswpout|pgmajfault' /proc/vmstat
echo "=== FOREGROUND ==="
activity=$(dumpsys activity activities | grep -E 'mResumedActivity|topResumedActivity' | head -2)
echo "$activity"
package=$(echo "$activity" | sed -n 's@.* \([a-zA-Z0-9_.]*\)/.*@\1@p' | head -1)
echo "=== CAPTURE_PROCESSES ==="
processes=$(ps -A -o PID,NAME,ARGS)
echo "$processes" | grep -E 'com.genymobile.scrcpy.Server|surfaceflinger|media.codec|media.swcodec|android.hardware.media.c2' | grep -v grep
servers=$(echo "$processes" | awk '/com.genymobile.scrcpy.Server/ && !/awk/ && !/grep/ {print $1}')
gamepids=""
case "$package" in
    ''|*[!a-zA-Z0-9_.]*) ;;
    *) gamepids=$(pidof "$package");;
esac
available=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
echo "MPH_SUMMARY available_kb=$available foreground=$package game_pids=$gamepids server_pids=$servers"
log -p i -t MPH_DIAG "available_kb=$available foreground=$package game_pids=$gamepids server_pids=$servers" 2>/dev/null
for pid in $servers $gamepids; do
    case "$pid" in ''|*[!0-9]*) continue;; esac
    echo "=== PROCESS $pid ==="
    cat /proc/$pid/status /proc/$pid/stat /proc/$pid/oom_score /proc/$pid/oom_score_adj 2>&1
    if [ "$1" = detail ]; then
        # Includes Java/native heap, PSS/RSS and graphics accounting where permitted.
        dumpsys meminfo "$pid"
    fi
done
if [ "$1" = detail ]; then
    echo "=== DEVICE ==="
    getprop ro.product.model
    getprop ro.build.version.release
    getprop ro.build.version.sdk
    echo "=== CODEC_MEMORY ==="
    dumpsys meminfo media.codec
    dumpsys meminfo media.swcodec
    dumpsys meminfo surfaceflinger
    echo "=== PROCESS_EXIT_HISTORY ==="
    # Also catches a game killed between samples after foreground returned to launcher.
    dumpsys activity exit-info
fi
