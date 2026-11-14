#!/usr/bin/env bash
# Build and flash one board over SWD (ST-LINK).
#
#   tools/flash.sh master          build the master firmware and flash it to the connected board
#   tools/flash.sh slave           same for the slave
#   tools/flash.sh slave --no-build    flash the existing build/Slave/telemetry.elf without rebuilding
#   tools/flash.sh master --sn 0015001041...   use this ST-LINK when several are connected
#   tools/flash.sh master --dry-run    show what would be done, flash nothing
#
# Connect the ST-LINK to the board you mean before running it: the script cannot tell which
# board is on the probe. Afterwards the board resets and starts the new firmware.
# Needs STM32CubeCLT (STM32_Programmer_CLI) on the PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

role=""
build=1
dry=0
sn=""
while [ $# -gt 0 ]; do
  case "$1" in
    master|Master) role=Master ;;
    slave|Slave) role=Slave ;;
    --no-build) build=0 ;;
    --dry-run) dry=1 ;;
    --sn) sn="$2"; shift ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
if [ -z "$role" ]; then
  echo "Which board? tools/flash.sh master   or   tools/flash.sh slave" >&2
  exit 2
fi

if ! command -v STM32_Programmer_CLI >/dev/null 2>&1; then
  echo "STM32_Programmer_CLI not found: install STM32CubeCLT and put its bin folders on the PATH." >&2
  exit 1
fi

if [ "$build" -eq 1 ]; then
  tools/build.sh "$role"
fi

elf="build/$role/telemetry.elf"
if [ ! -f "$elf" ]; then
  echo "$elf does not exist: run tools/build.sh $role first." >&2
  exit 1
fi

conn="port=SWD"
[ -n "$sn" ] && conn="$conn sn=$sn"
echo
echo "=== Flashing $role: $elf (built $(date -r "$elf" '+%Y-%m-%d %H:%M'))"
if [ "$dry" -eq 1 ]; then
  echo "dry run: STM32_Programmer_CLI -c $conn -w $elf -v -rst"
  exit 0
fi
# shellcheck disable=SC2086
STM32_Programmer_CLI -c $conn -w "$elf" -v -rst
echo
echo "$role flashed and restarted."
