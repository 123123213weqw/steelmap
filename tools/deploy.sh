#!/bin/bash
# 标准部署:WZU 静态编译 → 停旧进程 → scp → 重启(会中断当前对局)
set -e
cd "$(dirname "$0")/.."
echo "[1/4] sync source -> WZU"
rsync -a --exclude build --exclude vendor/SDL --exclude vendor/imgui \
	--exclude .DS_Store ./ WZU_Server:~/hoi/
echo "[2/4] static build on WZU"
ssh WZU_Server 'cd ~/hoi && cmake --build build_static --target server -j8 2>&1 | grep -cE " error"'
echo "[3/4] deploy to null (restarts game)"
ssh null@100.114.20.67 'pkill -f "\./server" || true; sleep 0.5'
scp -q WZU_Server:~/hoi/build_static/server null@100.114.20.67:~/steelmap/server
echo "[4/4] start"
ssh null@100.114.20.67 '~/steelmap/start.sh && sleep 0.5 && tail -1 ~/steelmap/server.log'
echo "deploy done"
