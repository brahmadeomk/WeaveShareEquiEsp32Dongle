#!/bin/sh
# Host-side check of the dongle firmware (no ESP32 needed):
#  1. type-checks the whole sketch against minimal Arduino/ESP32 stand-ins
#  2. runs the transaction-analyzer + diagnosis-engine tests in test.cpp
# Usage: sh tests/host/run.sh   (needs g++ and python3)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SKETCH="$HERE/../../UsbRs485Dongle"
OUT=$(mktemp -d)
cd "$OUT"
python3 -I "$HERE/build.py" "$SKETCH"
g++ -std=gnu++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -I "$HERE/stubs" -I "$SKETCH" sketch.cpp
cp "$HERE/test.cpp" .
g++ -std=gnu++17 -include vector -I "$HERE/stubs" -I "$SKETCH" test.cpp "$HERE/stubs.cpp" -o host_test
./host_test
