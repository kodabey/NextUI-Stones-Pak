#!/bin/sh
# Stones - Go for the TrimUI Brick (NextUI pak)
PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR" .pak)"
DATA="${SHARED_USERDATA_PATH:-/mnt/SDCARD/.userdata/shared}/$PAK_NAME"
LOGS="${LOGS_PATH:-$DATA}"
mkdir -p "$DATA" "$LOGS"

# schedutil instead of NextUI's default "performance" for paks: cooler, longer battery,
# and the AI still gets the full clock when it's thinking.
if [ -n "$SYSTEM_PATH" ] && [ -f "$SYSTEM_PATH/bin/governor.sh" ]; then
	sh "$SYSTEM_PATH/bin/governor.sh" auto >/dev/null 2>&1
fi

# keep NextUI's brightness/volume settings in sync
command -v syncsettings.elf >/dev/null 2>&1 && syncsettings.elf &

export STONES_DATA="$DATA"
export LD_LIBRARY_PATH="$PAK_DIR/lib:${LD_LIBRARY_PATH:-/usr/lib}"
cd "$PAK_DIR"
chmod +x ./stones 2>/dev/null
./stones > "$LOGS/$PAK_NAME.txt" 2>&1
