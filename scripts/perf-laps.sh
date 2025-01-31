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
# ----------------------------------------------------------------

printf -v START_TIME '%(%Y-%m-%dT%H:%M:%S)T' -1

PERF_FILE=/tmp/laps.$START_TIME.perf
SCRIPT_FILE=/tmp/laps.$START_TIME.script
SVG_FILE=~/laps.$START_TIME
PERF_DURATION=30

lapsPID=$(pidof lapsRelay)
tids=$(ps -T -p $lapsPID -o spid --no-headers)

### run_perf <tid>
function run_perf() {
    tid=$1

    echo "Running perf on LAPS for $PERF_DURATION seconds, output file $PERF_FILE.${tid}"
    sudo perf record -g -p $lapsPID -t $tid -o $PERF_FILE.${tid} sleep $PERF_DURATION

    echo "Running perf script $PERF_FILE.${tid} to $SCRIPT_FILE.${tid}"
    sudo perf script -i $PERF_FILE.${tid} > $SCRIPT_FILE.${tid}

    echo "Creating flame graph, output to $SVG_FILE.${tid}.svg"

    ~/FlameGraph/stackcollapse-perf.pl $SCRIPT_FILE.${tid} | ~/FlameGraph/flamegraph.pl \
            --title "LAPS (pid=$lapsPID) Thread=${tid} $START_TIME"
            --subtitle "${1}" > $SVG_FILE.${tid}.svg

    sudo rm -f $PERF_FILE.${tid} $SCRIPT_FILE.${tid}
    echo "Done with tid ${tid}"
}

#### Main

for tid in $tids; do
    run_perf $tid &
done

# wait for the perf processes to end
sleep $((PERF_DURATION + 2))
