/* M0/M1 客户端:
 *   headless(默认):连接 → 选座 → 按脚本发命令 → 报告最终哈希(CI/验收用)
 *   --render:      打开 SDL 窗口,画省份/单位,左键点省下移动令
 *   --screenshot P --shot-at N:渲染模式下到达 tick N 抓帧存 BMP 后退出(自动验证)
 *
 * 线程模型:XRT worker 线程收快照写 SharedView;渲染/主线程读副本;
 * UI 发命令经 after(0) 转回 worker 线程发送。
 */
#include "../core/command.hpp"
#include "../core/serialize.hpp"
#include "../core/state.hpp"
#include "../net/protocol.hpp"
#include "../net/xrt_transport.hpp"
#include "render.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <tuple>
#include <vector>

using namespace gs;

namespace {

struct ClientApp {
	std::vector<std::tuple<uint32_t, uint16_t, uint16_t>> script;
	size_t scriptIdx = 0;
	uint32_t untilTick = 40;
	uint16_t seat = 0xFFFF;

	std::atomic<int> myPlayer{ -1 };
	bool wantStart = false;                    /* --start:入座后自动开局 */
	std::atomic<uint32_t> lastTick{ 0 };
	std::atomic<uint64_t> lastHash{ 0 };
	std::atomic<bool> done{ false };
	SharedView* view = nullptr;             // render 模式非空
	XrtTransport* transport = nullptr;

	void onMessage(ConnId, const uint8_t* data, size_t size)
	{
		Msg m;
		if (!decodeMsg(data, size, m)) {
			return;
		}
		switch (m.type) {
		case MsgType::Accept:
			myPlayer.store((int)m.player);
			printf("[client] seated as player %u\n", (unsigned)m.player);
			if (wantStart && m.player == 0 && transport != nullptr) {
				XrtTransport* t = transport;
				t->after(0, [t]() {
					Command g;
					g.type = CmdType::StartGame;
					g.player = 0;
					std::vector<uint8_t> out;
					encodeCommand(out, g);
					(void)t->send(1, out.data(), out.size());
				});
			}
			break;
		case MsgType::Event:
			if (view != nullptr) {
				std::lock_guard<std::mutex> lk(view->mu);
				EventLine e;
				e.type = m.evType;
				e.player = m.evPlayer;
				e.a = m.evA;
				e.b = m.evB;
				e.day = (uint32_t)lastTick.load();
				view->events.push_back(e);
				if (view->events.size() > 60) {
					view->events.erase(view->events.begin());
				}
			}
			break;
		case MsgType::Snapshot: {
			GameState st;
			if (!deserialize(m.state.data(), m.state.size(), st)) {
				printf("[client] FAIL: bad snapshot %zu bytes:", m.state.size());
				for (size_t q = 0; q < 24 && q < m.state.size(); q++) {
					printf(" %02x", m.state[q]);
				}
				printf("\n");
				done.store(true);
				return;
			}
			const uint64_t h = stateHash(st);
			lastTick.store(st.tick);
			lastHash.store(h);
			if (view != nullptr) {
				std::lock_guard<std::mutex> lk(view->mu);
				view->state = st;
				view->hash = h;
				view->has = true;
				view->snapSeq++;
			}
			if (view == nullptr) {           // headless:脚本驱动
				while (scriptIdx < script.size()
					&& std::get<0>(script[scriptIdx]) <= st.tick) {
					Command c;
					c.type = CmdType::MoveUnit;
					c.player = (PlayerId)myPlayer.load();
					c.unit = std::get<1>(script[scriptIdx]);
					c.target = std::get<2>(script[scriptIdx]);
					std::vector<uint8_t> out;
					encodeCommand(out, c);
					(void)transport->send(1, out.data(), out.size());
					scriptIdx++;
				}
				if (st.tick >= untilTick) {
					printf("[client] final: tick=%u hash=%016llx\n",
						(unsigned)st.tick, (unsigned long long)h);
					done.store(true);
				}
			}
			break;
		}
		default:
			break;
		}
	}

	void onConn(ConnId, ConnEvent e)
	{
		if (e == ConnEvent::Opened) {
			std::vector<uint8_t> out;
			encodeJoin(out, seat);
			(void)transport->send(1, out.data(), out.size());
		}
	}
};

} // namespace

int main(int argc, char** argv)
{
	ClientApp app;
	const char* host = "127.0.0.1";
	uint16_t port = 47654;
	const char* scriptPath = nullptr;
	bool renderMode = false;
	const char* shotPath = nullptr;
	uint32_t shotAtTick = 20;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc) {
			host = argv[++i];
		} else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			port = (uint16_t)strtoul(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--seat") && i + 1 < argc) {
			app.seat = (uint16_t)strtoul(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--until") && i + 1 < argc) {
			app.untilTick = (uint32_t)strtoul(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
			scriptPath = argv[++i];
		} else if (!strcmp(argv[i], "--render")) {
			renderMode = true;
		} else if (!strcmp(argv[i], "--start")) {
			app.wantStart = true;
		} else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
			shotPath = argv[++i];
		} else if (!strcmp(argv[i], "--shot-at") && i + 1 < argc) {
			shotAtTick = (uint32_t)strtoul(argv[++i], nullptr, 10);
		}
	}

	if (scriptPath != nullptr) {
		std::ifstream f(scriptPath);
		uint32_t tick;
		unsigned unit, target;
		while (f >> tick >> unit >> target) {
			app.script.emplace_back(tick, (uint16_t)unit,
				(uint16_t)target);
		}
	}

	SharedView view;
	if (renderMode) {
		app.view = &view;
	}

	XrtTransport transport;
	app.transport = &transport;
	transport.setMessageHandler(
		[&app](ConnId c, const uint8_t* d, size_t n) {
			app.onMessage(c, d, n);
		});
	transport.setConnEventHandler(
		[&app](ConnId c, ConnEvent e) { app.onConn(c, e); });

	if (!transport.connect(host, port)) {
		printf("[client] FAIL: connect %s:%u\n", host, (unsigned)port);
		return 1;
	}
	printf("[client] connecting to %s:%u ...\n", host, (unsigned)port);

	if (renderMode) {
		/* 等入座确认后开窗 */
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(5);
		while (app.myPlayer.load() < 0
			&& std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (app.myPlayer.load() < 0) {
			printf("[client] FAIL: no seat\n");
			return 2;
		}
		const int seat = app.myPlayer.load();
		/* UI 线程 → after(0) → worker 线程发送,全程不跨线程碰 XRT */
		auto sendOrder = [&transport, seat](uint16_t cmdType,
			uint16_t unit, uint16_t target) {
			Command c;
			c.type = (CmdType)cmdType;
			c.player = (PlayerId)seat;
			c.unit = unit;
			c.target = target;
			std::vector<uint8_t> out;
			encodeCommand(out, c);
			const std::vector<uint8_t> bytes = std::move(out);
			(void)transport.after(0, [&transport, bytes]() {
				(void)transport.send(1, bytes.data(), bytes.size());
			});
		};
		auto sendJoinFn = [&transport](uint16_t s) {
			std::vector<uint8_t> out;
			encodeJoin(out, s);
			const std::vector<uint8_t> bytes = std::move(out);
			(void)transport.after(0, [&transport, bytes]() {
				(void)transport.send(1, bytes.data(), bytes.size());
			});
		};
		(void)runRenderWindow(view, seat, sendOrder, sendJoinFn,
			shotPath, shotAtTick);
		printf("[client] final: tick=%u hash=%016llx\n",
			(unsigned)app.lastTick.load(),
			(unsigned long long)app.lastHash.load());
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		return 0;
	}

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(30);
	while (!app.done.load()
		&& std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (!app.done.load()) {
		printf("[client] FAIL: timeout at tick %u\n",
			(unsigned)app.lastTick.load());
		return 2;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return 0;
}
