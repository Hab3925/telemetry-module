#!/usr/bin/env bash
# Build the telemetry firmware for the master and/or the slave board.
#
#   tools/build.sh            build both boards
#   tools/build.sh master     build only the master   (build/Master/telemetry.elf)
#   tools/build.sh slave      build only the slave    (build/Slave/telemetry.elf)
#   tools/build.sh --clean    delete the build folders first and build from scratch
#
# Both boards must always run firmware built from the same source and app_config.h:
# the link settings and protocol version have to match. Build both after any change.
# Needs STM32CubeCLT (cmake, ninja, arm-none-eabi-gcc) on the PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

roles=()
clean=0
for arg in "$@"; do
  case "$arg" in
    master|Master) roles+=(Master) ;;
    slave|Slave) roles+=(Slave) ;;
    both) roles+=(Master Slave) ;;
    --clean) clean=1 ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown argument: $arg (use master, slave, both or --clean)" >&2; exit 2 ;;
  esac
done
[ ${#roles[@]} -eq 0 ] && roles=(Master Slave)

for tool in cmake ninja arm-none-eabi-gcc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "$tool not found: install STM32CubeCLT and put its bin folders on the PATH." >&2
    exit 1
  fi
done

for preset in "${roles[@]}"; do
  dir="build/$preset"
  echo "=== $preset"
  if [ "$clean" -eq 1 ]; then
    rm -rf "$dir"
  fi
  if [ ! -f "$dir/CMakeCache.txt" ]; then
    cmake --preset "$preset" >/dev/null
  fi
  cmake --build --preset "$preset"
done

echo
echo "Firmware ready:"
for preset in "${roles[@]}"; do
  elf="build/$preset/telemetry.elf"
  role=$(grep -m1 '^TELEMETRY_ROLE:' "build/$preset/CMakeCache.txt" | cut -d= -f2)
  printf '  %-6s %s  (role %s, built %s)\n' "$preset" "$elf" "$role" "$(date -r "$elf" '+%Y-%m-%d %H:%M')"
done
echo "Flash with: tools/flash.sh master   /   tools/flash.sh slave"
