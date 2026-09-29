/* M0 服务器:单房间,server/ 目录只有一个 main 是有意的——
 * 房间管理、大厅是 M4 的事,现在把"tick 循环 + 广播"做扎实。
 *
 * 线程模型:全部逻辑在 XRT worker 线程(命令、tick、广播),
 * 主线程只负责等待结束。无锁。
 *
 * 用法: server --port 47654 --days 40 --ms 100 --grace 2500 \
 *            --seed 42 --replay /tmp/replay.txt
 */
#include "../core/command.hpp"
#include "../core/mapfile.hpp"
#include "../core/serialize.hpp"
#include "../core/tick.hpp"
#include "../core/state.hpp"
#include "../core/tick.hpp"
#include "../net/protocol.hpp"
#include "../net/xrt_transport.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include <netinet/in.h>
#include <unistd.h>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gs;

namespace {

constexpr uint16_t MAX_PLAYERS = 4;         // M0:4 个座位,0-3 各拥一单位

struct ServerApp {
	GameState state;
	std::vector<Command> pending;            // worker 线程内:收命令→入队
	std::map<ConnId, PlayerId> connPlayer;
	std::vector<bool> seatTaken{ false, false, false, false };
	std::vector<std::string> replayLines;    // "tick unit target"
	uint64_t seed = 42;
	uint32_t days = 40;
	int aiPlayers = 0;                /* --ai N:座位 1..N 由机器人接管 */
	uint64_t graceMs = 2500;
	const char* replayPath = nullptr;
	XrtTransport* transport = nullptr;
	std::atomic<bool> done{ false };
	bool fog = true;                          /* --nofog 关闭 */
	std::recursive_mutex tickMu;                        /* IO 线程与游戏循环线程的串行化 */
	uint16_t listenPort = 0;
	/* 发件箱:游戏线程生产,worker(IO 回调)消费——XRT 的 send
	 * 仅在 worker 内有效,timer/post 唤醒跨平台不可靠,自连踢是
	 * 唯一全平台保证的 worker 唤醒方式 */
	std::vector<std::pair<ConnId, std::vector<uint8_t>>> outbox;
	std::vector<std::vector<uint8_t>> broadcastBox;

	void kick()
	{
		/* 连接即踢:accept 的 Opened 事件(worker)就是唤醒,
		 * 数据会被 RST 吞所以根本不发。 */
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			return;
		}
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(listenPort);
		a.sin_addr.s_addr = htonl(0x7F000001);
		(void)connect(fd, (sockaddr*)&a, sizeof(a));
		usleep(2000);                     /* 让 accept/事件先完成 */
		close(fd);
	}

	/* 按观察者过滤的快照:己方单位/省份+其邻省可见,
	 * 其余省份归属显中立、敌军置 fog(客户端不渲染)。
	 * 权威状态与回放不受影响。 */
	GameState maskedFor(PlayerId viewer) const
	{
		GameState v = state;
		if (!fog || viewer == SERVER_PLAYER || viewer >= 16) {
			return v;
		}
		std::vector<uint8_t> vis(v.map.count, 0);
		for (const Unit& u : v.units) {
			if (u.strength > 0 && u.owner == viewer
				&& u.province < v.map.count) {
				vis[u.province] = 1;
				for (ProvinceId n : v.map.adj[u.province]) {
					vis[n] = 1;
				}
			}
		}
		for (uint16_t p = 0; p < v.map.count; p++) {
			if (p < v.provinceOwner.size()
				&& v.provinceOwner[p] == viewer) {
				vis[p] = 1;
				for (ProvinceId n : v.map.adj[p]) {
					vis[n] = 1;
				}
			}
		}
		for (uint16_t p = 0; p < v.map.count; p++) {
			if (!vis[p] && p < v.provinceOwner.size()) {
				v.provinceOwner[p] = NEUTRAL_PLAYER;
			}
		}
		for (Unit& u : v.units) {
			if (u.owner != viewer && u.province < v.map.count
				&& !vis[u.province]) {
				u.fog = 1;
			}
		}
		return v;
	}

	void broadcastSnapshots()
	{
		/* 快照按观察者生成入广播箱,由 worker 的 flushBoxes 发送 */
		std::vector<std::pair<ConnId, PlayerId>> conns(
			connPlayer.begin(), connPlayer.end());
		broadcastBox.clear();
		for (const auto& kv : conns) {
			std::vector<uint8_t> bytes;
			serialize(maskedFor(kv.second), bytes);
			std::vector<uint8_t> msg;
			encodeSnapshot(msg, bytes);
			broadcastBox.push_back(std::move(msg));
		}
	}

	void flushBoxes(ConnId kickConn)
	{
		for (const auto& kv : outbox) {
			(void)transport->send(kv.first, kv.second.data(),
				kv.second.size());
		}
		outbox.clear();
		for (const auto& msg : broadcastBox) {
			transport->broadcast(msg.data(), msg.size());
		}
		broadcastBox.clear();
	}

	void lobbyTick()
	{
		std::lock_guard<std::recursive_mutex> lk(tickMu);
		/* 大厅:处理 StartGame 令并广播(由 gameLoop 线程驱动) */
		for (const Command& cmd : pending) {
			if (cmd.type == CmdType::StartGame
				&& validateCommand(state, cmd)) {
				applyCommand(state, cmd);
				replayLines.push_back("0 G 0");
				printf("[server] game started by host\n");
			}
		}
		pending.clear();
		broadcastSnapshots();
	}

	/* 独立游戏循环线程:不再依赖 XRT 定时器(Linux 后端不可靠,
	 * Mac 正常 Linux 静默丢拍,已实测;朴素 sleep 最稳) */
	void gameLoop(uint64_t graceMs)
	{
		std::this_thread::sleep_for(
			std::chrono::milliseconds(graceMs));
		while (!done.load()) {
			if (state.phase == 0) {
				lobbyTick();
				kick();
				std::this_thread::sleep_for(
					std::chrono::milliseconds(500));
			} else {
				onDay();
				kick();
				if (!done.load()) {
					std::this_thread::sleep_for(
						std::chrono::milliseconds(
							(long long)msPerDay));
				}
			}
		}
	}

	void onMessage(ConnId c, const uint8_t* data, size_t size)
	{
		std::lock_guard<std::recursive_mutex> lk(tickMu);
		Msg m;
		if (!decodeMsg(data, size, m)) {
			return;
		}
		switch (m.type) {
		case MsgType::Join: {
			PlayerId seat = SERVER_PLAYER;
			if (m.seat < MAX_PLAYERS && !seatTaken[m.seat]) {
				seat = m.seat;           // 指定座位且空闲
			} else {
				for (PlayerId i = 0; i < MAX_PLAYERS; i++) {
					if (!seatTaken[i]) {
						seat = i;
						break;
					}
				}
			}
			/* 大厅里允许换座:先释放旧座 */
			const auto old = connPlayer.find(c);
			if (old != connPlayer.end() && state.phase == 0) {
				const PlayerId prev = old->second;
				if (prev != SERVER_PLAYER
					&& (prev == 0 || prev > (PlayerId)aiPlayers)) {
					seatTaken[prev] = false;
				}
				if (seat == SERVER_PLAYER) {
					seat = prev;         /* 没有空位:保留原座 */
				}
			}
			if (seat != SERVER_PLAYER) {
				seatTaken[seat] = true;
			}
			connPlayer[c] = seat;
			std::vector<uint8_t> out;
			encodeAccept(out, seat);
			(void)transport->send(c, out.data(), out.size());
			printf("[server] conn %u seated as player %u\n",
				(unsigned)c, (unsigned)seat);
			break;
		}
		case MsgType::Command: {
			/* 反欺骗关键行:player 由连接映射决定,客户端说什么不算 */
			const auto it = connPlayer.find(c);
			if (it == connPlayer.end()) {
				return;
			}
			Command cmd = m.cmd;
			cmd.player = it->second;
			if (validateCommand(state, cmd)) {
				pending.push_back(cmd);
			} else {
				printf("[server] REJECT conn %u: cmd %u invalid\n",
					(unsigned)c, (unsigned)(int)cmd.type);
			}
			break;
		}
		default:
			break;
		}
	}

	void onConn(ConnId c, ConnEvent e)
	{
		std::lock_guard<std::recursive_mutex> lk(tickMu);
		if (e == ConnEvent::Opened) {
			flushBoxes(c);        /* 任何新连接(含自连踢)都冲刷 */
			return;
		}
		if (e == ConnEvent::Closed) {
			flushBoxes(c);                /* 关闭也冲刷(自连短命) */
			const auto it = connPlayer.find(c);
			if (it != connPlayer.end()) {
				const PlayerId seat = it->second;
				connPlayer.erase(it);
				/* 断线释放人类座位 → 同座位可重连(快照即续局)。
				 * 1..aiPlayers 是 AI 座,不释放;0 号永不为 AI。 */
				if (seat != SERVER_PLAYER
					&& (seat == 0 || seat > (PlayerId)aiPlayers)) {
					seatTaken[seat] = false;
				}
				printf("[server] conn %u left, seat %u %s\n",
					(unsigned)c, (unsigned)seat,
					(seat != SERVER_PLAYER
						&& (seat == 0 || seat > (PlayerId)aiPlayers))
						? "released" : "kept(AI)");
			}
		}
	}

	/* 机器人:每天给空闲的己方单位下"开赴最近非己方省"的令。
	 * AI 走命令通道 → 进回放日志 → 离线重演与在线一致。
	 */
	void genAiCommands()
	{
		for (int a = 1; a <= aiPlayers && a < 16; a++) {
			std::vector<ProvinceId> goals;
			for (uint16_t p = 0; p < state.map.count; p++) {
				if (state.provinceOwner[p] != (PlayerId)a) {
					goals.push_back((ProvinceId)p);
				}
			}
			if (goals.empty()) {
				continue;
			}
			const std::vector<uint32_t> dist =
				distanceFromSet(state.map, goals);
			for (Unit& u : state.units) {
				if (u.owner != (PlayerId)a || u.strength <= 0
					|| u.target != INVALID_PROVINCE) {
					continue;
				}
				const uint32_t here = dist[u.province];
				if (here == 0) {
					continue;            /* 已站在目标省(战斗中) */
				}
				uint16_t cur = u.province;
				while (dist[cur] > 0) {
					ProvinceId step = INVALID_PROVINCE;
					for (ProvinceId n : state.map.adj[cur]) {
						if (dist[n] == dist[cur] - 1
							&& (step == INVALID_PROVINCE
								|| n < step)) {
							step = n;
						}
					}
					if (step == INVALID_PROVINCE) {
						break;
					}
					cur = step;
				}
				Command c;
				c.type = CmdType::MoveUnit;
				c.player = (PlayerId)a;
				c.unit = u.id;
				c.target = cur;
				if (validateCommand(state, c)) {
					pending.push_back(c);
				}
			}
		}
	}

	/* AI 造兵:补给够且兵不多时,在最小 id 己方省造一个 */
	void genAiBuild()
	{
		for (int a = 1; a <= aiPlayers && a < 16; a++) {
			int alive = 0;
			for (const Unit& u : state.units) {
				if (u.owner == (PlayerId)a && u.strength > 0) {
					alive++;
				}
			}
			if (alive >= 4 || state.supplies[a] < COST_BUILD) {
				continue;
			}
			for (uint16_t p = 0; p < state.map.count; p++) {
				if (state.provinceOwner[p] == (PlayerId)a) {
					Command c;
					c.type = CmdType::BuildUnit;
					c.player = (PlayerId)a;
					c.target = p;
					if (validateCommand(state, c)) {
						pending.push_back(c);
					}
					break;
				}
			}
		}
	}

	/* 胜负:只剩一家有存活单位 */
	int checkWinner()
	{
		PlayerId alive = SERVER_PLAYER;
		bool contested = false;
		for (const Unit& u : state.units) {
			if (u.strength > 0) {
				if (alive == SERVER_PLAYER) {
					alive = u.owner;
				} else if (u.owner != alive) {
					contested = true;
					break;
				}
			}
		}
		return contested ? -1 : (int)alive;
	}

	/* 快照前状态,用于生成事件 */
	std::vector<PlayerId> prevOwners;
	std::vector<int32_t> prevStrength;
	size_t prevUnits = 0;

	void emitEvents()
	{
		auto send1 = [&](uint16_t t, uint16_t pl, uint16_t a,
			uint16_t b) {
			std::vector<uint8_t> m;
			encodeEvent(m, t, pl, a, b);
			broadcastBox.push_back(std::move(m));
		};
		if (state.units.size() > prevUnits) {
			for (size_t i = prevUnits; i < state.units.size(); i++) {
				send1((uint16_t)EventType::Built,
					state.units[i].owner, (uint16_t)i, 0);
			}
		}
		for (size_t i = 0; i < state.units.size() && i < prevStrength.size();
			i++) {
			if (prevStrength[i] > 0 && state.units[i].strength == 0) {
				send1((uint16_t)EventType::Died,
					state.units[i].owner, (uint16_t)i, 0);
			}
		}
		for (uint16_t p = 0; p < state.map.count && p < prevOwners.size();
			p++) {
			if (prevOwners[p] != state.provinceOwner[p]) {
				send1((uint16_t)EventType::Capture,
					state.provinceOwner[p], p, prevOwners[p]);
			}
		}
	}

	/* 一个 tick = 一天:AI 令 → 结算命令 → 推进 → 广播快照 */
	uint64_t msPerDay = 100;
	bool paused = false;

	void onDay()
	{
		std::lock_guard<std::recursive_mutex> lk(tickMu);
		if (paused) {                     /* 暂停:跳过仿真,保持响应 */
			return;
		}
		genAiCommands();
		genAiBuild();
		prevOwners = state.provinceOwner;
		prevStrength.clear();
		for (const Unit& u : state.units) {
			prevStrength.push_back(u.strength);
		}
		prevUnits = state.units.size();
		for (const Command& cmd : pending) {
			if (cmd.type == CmdType::SetSpeed) {
				msPerDay = cmd.target;
				paused = (cmd.target == 0);
				printf("[server] speed: %llums/day%s\n",
					(unsigned long long)msPerDay,
					paused ? " (PAUSED)" : "");
				continue;              /* 节奏命令:不进仿真与回放 */
			}
			const uint32_t atTick = state.tick;
			applyCommand(state, cmd);
			char line[64];
			if (cmd.type == CmdType::BuildUnit) {
				snprintf(line, sizeof(line), "%u B %u", atTick,
					(unsigned)cmd.target);
			} else {
				snprintf(line, sizeof(line), "%u %u %u", atTick,
					(unsigned)cmd.unit, (unsigned)cmd.target);
			}
			replayLines.push_back(line);
		}
		pending.clear();
		stepDay(state);

		broadcastSnapshots();

		emitEvents();
		const int winner = checkWinner();
		if (winner >= 0 || state.tick >= days) {
			if (winner >= 0) {
				printf("[server] WINNER: player %d at tick %u\n",
					winner, (unsigned)state.tick);
				std::vector<uint8_t> out;
				encodeEvent(out, (uint16_t)EventType::Winner,
					(uint16_t)winner, 0, 0);
				broadcastBox.push_back(std::move(out));
			}
			printf("[server] done: tick=%u hash=%016llx players=%zu\n",
				(unsigned)state.tick,
				(unsigned long long)stateHash(state),
				connPlayer.size());
			dumpReplay();
			done.store(true);
			return;
		}
	}

	void dumpReplay()
	{
		if (replayPath == nullptr) {
			return;
		}
		std::ofstream f(replayPath);
		if (!f) {
			printf("[server] cannot write replay: %s\n", replayPath);
			return;
		}
		f << seed << "\n";
		for (const std::string& l : replayLines) {
			f << l << "\n";
		}
	}
};

} // namespace

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);   /* 常驻服务:日志行缓冲 */
	ServerApp app;
	uint16_t port = 47654;
	const char* mapPath = nullptr;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			port = (uint16_t)strtoul(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--days") && i + 1 < argc) {
			app.days = (uint32_t)strtoul(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--ms") && i + 1 < argc) {
			app.msPerDay = strtoull(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--grace") && i + 1 < argc) {
			app.graceMs = strtoull(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			app.seed = strtoull(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--replay") && i + 1 < argc) {
			app.replayPath = argv[++i];
		} else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
			mapPath = argv[++i];
		} else if (!strcmp(argv[i], "--ai") && i + 1 < argc) {
			app.aiPlayers = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--nofog")) {
			app.fog = false;
		}
	}

	if (mapPath != nullptr) {
		if (!loadMapFile(mapPath, app.state)) {
			printf("[server] FAIL: cannot load map %s\n", mapPath);
			return 1;
		}
		printf("[server] map loaded: %s (%u provinces)\n", mapPath,
			(unsigned)app.state.map.count);
	} else {
		app.state.buildDefaultMap(32);
	}
	app.state.phase = 0;                         /* 先在大厅,房主开局 */
	app.state.seedRng(app.seed);
	for (UnitId i = 0; i < MAX_PLAYERS; i++) {
		Unit u;
		u.id = i;
		u.owner = (PlayerId)i;
		u.province = (ProvinceId)((i * (int)app.state.map.count) / 4
			% app.state.map.count);
		app.state.units.push_back(u);
	}

	XrtTransport transport;
	app.transport = &transport;
	transport.setMessageHandler(
		[&app](ConnId c, const uint8_t* d, size_t n) {
			app.onMessage(c, d, n);
		});
	transport.setConnEventHandler(
		[&app](ConnId c, ConnEvent e) { app.onConn(c, e); });

	app.state.phase = 0;
	for (int a = 1; a <= app.aiPlayers && a < 16; a++) {
		app.seatTaken[a] = true;        /* AI 座位,人类不可入座 */
	}
	app.listenPort = port;
	if (!transport.listen(port)) {
		printf("[server] FAIL: listen %u\n", (unsigned)port);
		return 1;
	}
	printf("[server] listening on 0.0.0.0:%u, %u days, %llums/day, "
		"grace %llums\n",
		(unsigned)port, (unsigned)app.days,
		(unsigned long long)app.msPerDay,
		(unsigned long long)app.graceMs);

	/* 独立线程驱动大厅/对局循环 */
	std::thread loop([&app]() { app.gameLoop(app.graceMs); });

	while (!app.done.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (loop.joinable()) {
		loop.join();
	}
	/* 给最后的快照一点冲刷时间再销毁引擎 */
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	return 0;
}
