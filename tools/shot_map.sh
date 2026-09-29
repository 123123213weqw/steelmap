#!/bin/bash
# 地图渲染验收:server(指定地图) + 双 client + 截图
# 用法: shot_map.sh [port]
set -u
cd "$(dirname "$0")/.."
PORT=${1:-47907}
MAP=${2:-maps/voronoi64.map}
LOG=/tmp/m0/shot
mkdir -p $LOG
rm -f $LOG/*

./build/server --port $PORT --days 120 --ms 300 --grace 2500 --seed 42 \
	--map "$MAP" --replay $LOG/replay.txt > $LOG/server.log 2>&1 &
SPID=$!
sleep 0.8

printf "3 1 5\n" > $LOG/b.txt
./build/client --host 127.0.0.1 --port $PORT --seat 1 \
	--script $LOG/b.txt --until 200 > $LOG/cb.log 2>&1 &
CB=$!

./build/client --host 127.0.0.1 --port $PORT --seat 0 --render \
	--screenshot /tmp/m0_shot.bmp --shot-at 25 > $LOG/ca.log 2>&1
RA=$?

kill $CB 2>/dev/null
kill $SPID 2>/dev/null
wait 2>/dev/null

rm -f /tmp/m0_shot.png
sips -s format png /tmp/m0_shot.bmp --out /tmp/m0_shot.png >/dev/null 2>&1

echo "render_exit=$RA"
grep -E "map loaded|ORDER accepted|done:" $LOG/server.log | head -4
grep -E "seated|screenshot" $LOG/ca.log | head -3
ls -la /tmp/m0_shot.png 2>/dev/null
