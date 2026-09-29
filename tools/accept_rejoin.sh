#!/bin/bash
# 断线重连验收:A 进场 → 断开 → A 同座位重入 → 状态连续
set -u
cd "$(dirname "$0")/.."
PORT=${1:-47990}
LOG=/tmp/m0/rejoin
mkdir -p $LOG; rm -f $LOG/*

./build/server --port $PORT --days 40 --ms 100 --grace 2000 --seed 42 --nofog \
	> $LOG/server.log 2>&1 &
SPID=$!
sleep 0.6
./build/client --host 127.0.0.1 --port $PORT --seat 0 --start --until 8 \
	> $LOG/a1.log 2>&1
echo "first session: $(grep -c 'seated' $LOG/a1.log) seated, $(grep -oE 'hash=[0-9a-f]+' $LOG/a1.log | tail -1)"
sleep 1
./build/client --host 127.0.0.1 --port $PORT --seat 0 --start --until 16 \
	> $LOG/a2.log 2>&1
RC=$?
kill $SPID 2>/dev/null; wait 2>/dev/null
echo "rejoin rc=$RC, seated=$(grep -c 'seated as player 0' $LOG/a2.log)"
echo "rejoin final: $(grep -oE 'hash=[0-9a-f]+' $LOG/a2.log | tail -1)"
grep -E "left, seat|seated as" $LOG/server.log | head -4
if grep -q "seated as player 0" $LOG/a2.log && grep -q "released" $LOG/server.log; then
	echo "===== REJOIN: PASS ====="
else
	echo "===== REJOIN: FAIL ====="
fi
