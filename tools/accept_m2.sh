#!/bin/bash
# M2 验收:AI 战争 + 归属上色 + 回放对账(setvbuf 后无需 pty)
set -u
cd "$(dirname "$0")/.."
PORT=${1:-48010}
LOG=/tmp/m0/m2
mkdir -p $LOG; rm -rf $LOG/*
DAYS=60

./build/server --port $PORT --days $DAYS --ms 100 --grace 2500 --seed 42 \
	--nofog --ai 1 --map maps/voronoi64.map --replay $LOG/replay.txt \
	> $LOG/server.log 2>&1 &
SPID=$!
sleep 0.8

./build/client --host 127.0.0.1 --port $PORT --seat 0 --start --render \
	--screenshot /tmp/m2_shot.bmp --shot-at 45 > $LOG/ca.log 2>&1
RA=$?
wait $SPID 2>/dev/null

sips -s format png /tmp/m2_shot.bmp --out /tmp/m2_shot.png >/dev/null 2>&1

echo "render_exit=$RA"
grep -aE "WINNER|done:" $LOG/server.log | head -3
grep -aE "seated|screenshot" $LOG/ca.log | head -3

SEED=$(head -1 $LOG/replay.txt)
tail -n +2 $LOG/replay.txt > $LOG/cmds.txt
echo "replay cmds: $(wc -l < $LOG/cmds.txt | tr -d ' ')"
HR=$(./build/sim_run --seed "$SEED" --days $DAYS --map maps/voronoi64.map \
	--script $LOG/cmds.txt | grep -oE '[0-9a-f]{16}' | tail -1)
HS=$(grep -aoE 'hash=[0-9a-f]+' $LOG/server.log | tail -1 | cut -d= -f2)
echo "hash: server=$HS replay=$HR"
[ -n "$HS" ] && [ "$HS" = "$HR" ] && echo "===== M2: PASS =====" \
	|| echo "===== M2: FAIL ====="
