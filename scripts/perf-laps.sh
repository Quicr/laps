#!/bin/bash
# ----------------------------------------------------------------
# Script to run linux perf and create a flame graph per thread
#    of the laps process. This script uses pidpof lapsRelay,
#    so only one laps process should be running. If more than
#    one is running, change pidof to the actual pid of the
#    laps process to perf.
#
# Flamegraph is expected to be in current user home dir, under ~/FlameGraph
#
# Install Notes:
#   sudo apt-get install -y linux-perf parallel git
#   git clone https://github.com/brendangregg/FlameGraph
#   ./perf-laps.sh
#
# LAPS must be built with the correct debug info for linux perf. Set
#  the below before running cmake (e.g. make build)
#
#    export CXXFLAGS="-fno-omit-frame-pointer -Wno-error=maybe-uninitialized -fno-inline -O0" CFLAGS="$CXXFLAGS"
#    export CXXFLAGS="-fno-omit-frame-pointer -Wno-error=maybe-uninitialized -fno-inline -Og -ggdb3" CFLAGS="$CXXFLAGS"
# ----------------------------------------------------------------

printf -v START_TIME '%(%Y-%m-%dT%H:%M:%S)T' -1

PERF_FILE=/tmp/laps.$START_TIME.perf
SCRIPT_FILE=/tmp/laps.$START_TIME.script
SVG_FILE=~/laps.$START_TIME
PERF_DURATION=30

lapsPID=$(pidof lapsRelay)
tids=$(ps -T -p $lapsPID -o tid --no-headers)

echo "Running perf on LAPS for $PERF_DURATION seconds, output file $PERF_FILE"
sudo perf record -g -p $lapsPID -o $PERF_FILE sleep $PERF_DURATION


### create_perf_script <tid>
function create_perf_script() {
    tid=$1
    echo "Running perf script $PERF_FILE.${tid} to $SCRIPT_FILE.${tid}"
    sudo perf script -i $PERF_FILE --tid=${tid} > $SCRIPT_FILE.${tid}

    echo "Creating flame graph, output to $SVG_FILE.${tid}.svg"

    ~/FlameGraph/stackcollapse-perf.pl $SCRIPT_FILE.${tid} | ~/FlameGraph/flamegraph.pl \
            --title "LAPS (pid=$lapsPID) Thread=${tid} $START_TIME" \
            --subtitle "${1}" > $SVG_FILE.${tid}.svg

    sudo rm -f $PERF_FILE $SCRIPT_FILE.${tid}
    echo "Done with tid ${tid}"
}

#### Main

for tid in $tids; do
    create_perf_script $tid &
done

echo "Done"
