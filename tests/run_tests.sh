#!/bin/bash
# Builds and runs every bench test. Needs python3, g++ and:  pip install gpiozero
# No Raspberry Pi and no headset are involved: the pedal runs on gpiozero's fake pins, and the
# headset's receiver is compiled for this machine with a stand-in for the Android log header.
set -e
cd "$(dirname "$0")"
APP=../Chapter2/app/src/main/cpp

echo "== 1. Pedal debounce, on a fake pin =="
python3 test_pedal.py

echo
echo "== 2. The relay and the headset's receiver, across a fake WiFi =="
g++ -std=c++17 -O2 -pthread -Istub -I$APP/network recv_harness.cpp $APP/network/PedalReceiver.cpp -o recv_harness
python3 test_link.py

echo
echo "== 3. A whole session through the experiment logic =="
g++ -std=c++17 -O1 -DNEW -I$APP exp/sim.cpp -o exp/sim_new
g++ -std=c++17 -O1 -Iexp/baseline exp/sim.cpp -o exp/sim_old
./exp/sim_old controller exp/old_ctrl.csv 1
./exp/sim_new controller exp/new_ctrl.csv 1 > /dev/null
./exp/sim_new pedal exp/new_pedal.csv 1 > /dev/null
if diff <(tail -n +2 exp/old_ctrl.csv) <(tail -n +2 exp/new_ctrl.csv) > /dev/null; then
    echo "  ok   controller input: $(tail -n +3 exp/new_ctrl.csv | wc -l) event rows identical to the build that recorded P01 and P02"
else
    echo "  FAIL controller input: the release build logs different events"; exit 1
fi
echo "  pedal session header: $(head -1 exp/new_pedal.csv)"
echo
echo "ALL TESTS PASSED"
