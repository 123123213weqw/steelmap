#!/bin/bash
# 玩家会话:服务器 + 渲染客户端,关掉窗口自动收摊
cd "$(dirname "$0")/.."
PORT=${1:-47654}
pkill -f "build/server --port $PORT" 2>/dev/null
sleep 0.3
MAP=maps/voronoi64.map
[ -f "$MAP" ] || ./build/mapgen --out "$MAP" --grid 8 --seed 7 >/dev/null
./build/server --port $PORT --days 100000 --ms 250 --grace 3000 --seed 42 --map "$MAP" --ai 1 &
SPID=$!
sleep 0.5
./build/client --host 127.0.0.1 --port $PORT --seat 0 --render &
CPID=$!
trap 'kill $SPID $CPID 2>/dev/null' EXIT
wait $CPID          # 窗口关闭后
kill $SPID 2>/dev/null
echo "session ended"
