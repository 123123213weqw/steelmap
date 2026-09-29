#!/bin/bash
# M0 联机验收:
#  1. server + 两个 client 进程,A 坐位 0、B 坐位 1
#  2. A 发一条越权命令(动 B 的单位)→ 服务器必须拒绝
#  3. 三方(服务器/A/B)最终哈希必须一致
#  4. 服务器回放日志用 sim_run 离线重演,哈希必须与在线一致
set -u
cd "$(dirname "$0")/.."

PORT=${1:-47890}
DAYS=40
LOG=/tmp/m0
mkdir -p $LOG
rm -f $LOG/*

./build/server --port $PORT --days $DAYS --ms 50 --grace 2500 \
	--seed 42 --nofog --replay $LOG/replay.txt > $LOG/server.log 2>&1 &
SPID=$!

sleep 0.8
printf "2 0 17\n3 1 5\n"  > $LOG/a.txt   # 第二条越权:unit 1 属于 B
printf "3 1 5\n10 1 12\n" > $LOG/b.txt

./build/client --host 127.0.0.1 --port $PORT --seat 0 --start \
	--script $LOG/a.txt --until $DAYS > $LOG/ca.log 2>&1 &
CA=$!
./build/client --host 127.0.0.1 --port $PORT --seat 1 \
	--script $LOG/b.txt --until $DAYS > $LOG/cb.log 2>&1 &
CB=$!

wait $CA; RA=$?
wait $CB; RB=$?
wait $SPID; RS=$?

echo "exit: server=$RS clientA=$RA clientB=$RB"
HS=$(grep -oE 'hash=[0-9a-f]+' $LOG/server.log | tail -1 | cut -d= -f2)
HA=$(grep -oE 'hash=[0-9a-f]+' $LOG/ca.log   | tail -1 | cut -d= -f2)
HB=$(grep -oE 'hash=[0-9a-f]+' $LOG/cb.log   | tail -1 | cut -d= -f2)
echo "hash: server=$HS A=$HA B=$HB"

FAIL=0
[ "$RA" = 0 ] && [ "$RB" = 0 ] && [ "$RS" = 0 ] || FAIL=1
[ -n "$HS" ] && [ "$HS" = "$HA" ] && [ "$HA" = "$HB" ] || FAIL=1

if grep -q "REJECT" $LOG/server.log; then
	echo "越权拒绝: OK"
else
	echo "越权拒绝: MISSING"
	FAIL=1
fi

# 离线回放对账:在线状态 == 回放状态
SEED=$(head -1 $LOG/replay.txt)
tail -n +2 $LOG/replay.txt > $LOG/cmds.txt
HR=$(./build/sim_run --seed "$SEED" --days $DAYS --script $LOG/cmds.txt \
	| grep -oE '[0-9a-f]{16}' | tail -1)
echo "replay: seed=$SEED hash=$HR"
[ -n "$HR" ] && [ "$HR" = "$HS" ] || FAIL=1

if [ "$FAIL" = 0 ]; then
	echo "===== M0 ACCEPTANCE: PASS ====="
else
	echo "===== M0 ACCEPTANCE: FAIL ====="
fi
exit $FAIL
