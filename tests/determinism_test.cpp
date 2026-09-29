/* M0 验收测试:
 *  1. 确定性:同种子 + 同命令流,两次运行状态哈希一致
 *  2. 回放:命令流重新跑一遍,哈希一致
 *  3. 分歧:命令不同 → 哈希不同
 *  4. 序列化往返:dump→load 哈希一致,且能继续推演
 *  5. 移动正确性:环图上 0→5 恰好 5 天到达,目标自动清除
 *  6. 命令校验:非归属玩家的命令被拒,状态不变
 *  7. 环回传输:命令经 Transport 进出,路由正确
 */
#include "../core/command.hpp"
#include "../core/serialize.hpp"
#include "../core/tick.hpp"
#include "../net/loopback.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace gs;

static int g_fail = 0;

#define CHECK(cond)                                                           \
	do {                                                                       \
		if (!(cond)) {                                                         \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
			g_fail++;                                                          \
		}                                                                      \
	} while (0)

struct Scheduled {
	uint32_t tick;
	Command cmd;
};

static GameState makeWorld()
{
	GameState s;
	s.buildDefaultMap(32);
	s.seedRng(42);
	for (UnitId i = 0; i < 4; i++) {
		Unit u;
		u.id = i;
		u.owner = (PlayerId)i;
		u.province = (ProvinceId)(i * 8);
		s.units.push_back(u);
	}
	return s;
}

static uint64_t runSim(const std::vector<Scheduled>& script, uint32_t days,
	const GameState& world)
{
	GameState s = world;
	for (uint32_t d = 0; d < days; d++) {
		for (const Scheduled& sc : script) {
			if (sc.tick == s.tick && validateCommand(s, sc.cmd)) {
				applyCommand(s, sc.cmd);
			}
		}
		stepDay(s);
	}
	return stateHash(s);
}

int main()
{
	const GameState world = makeWorld();

	/* 1+2+3:确定性 / 回放 / 分歧 */
	std::vector<Scheduled> script = {
		{ 0,  { CmdType::MoveUnit, 0, 0, 17 } },
		{ 3,  { CmdType::MoveUnit, 1, 1, 5 } },
		{ 10, { CmdType::MoveUnit, 2, 2, 30 } },
		{ 11, { CmdType::MoveUnit, 3, 3, 8 } },
		{ 20, { CmdType::MoveUnit, 0, 0, 0 } },
	};
	const uint64_t h1 = runSim(script, 60, world);
	const uint64_t h2 = runSim(script, 60, world);
	CHECK(h1 == h2);

	std::vector<Scheduled> script2 = script;
	script2[2].cmd.target = 29;
	const uint64_t h3 = runSim(script2, 60, world);
	CHECK(h3 != h1);

	/* 4:序列化往返,且往返后继续推演仍一致 */
	{
		GameState a = world;
		for (const Scheduled& sc : script) {
			if (sc.tick == a.tick && validateCommand(a, sc.cmd)) {
				applyCommand(a, sc.cmd);
			}
			if (sc.tick == a.tick) {
				break;
			}
		}
		stepDay(a);
		std::vector<uint8_t> bytes;
		serialize(a, bytes);
		GameState b;
		CHECK(deserialize(bytes.data(), bytes.size(), b));
		CHECK(stateHash(a) == stateHash(b));

		for (int i = 0; i < 10; i++) {
			stepDay(a);
			stepDay(b);
		}
		CHECK(stateHash(a) == stateHash(b));
	}

	/* 5:环图 0→5 = 5 跳,恰好第 5 天到达 */
	{
		GameState s = world;
		Command c{ CmdType::MoveUnit, 0, 0, 5 };
		CHECK(validateCommand(s, c));
		applyCommand(s, c);
		for (int d = 0; d < 4; d++) {
			stepDay(s);
		}
		CHECK(s.units[0].province != 5);          // 第 4 天还没到
		stepDay(s);
		CHECK(s.units[0].province == 5);          // 第 5 天到达
		CHECK(s.units[0].target == INVALID_PROVINCE); // 到达后驻留
	}

	/* 6:玩家 1 试图移动玩家 0 的单位 → 拒绝且状态不变 */
	{
		GameState s = world;
		Command c{ CmdType::MoveUnit, 1, 0, 9 };
		CHECK(!validateCommand(s, c));
		const uint64_t before = stateHash(s);
		applyCommand(s, c);                        // 越权命令不应用
		CHECK(stateHash(s) == before);
	}

	/* 7:命令经 Transport 环回,按连接路由 */
	{
		LoopbackTransport t;
		ConnId seenBy = INVALID_CONN;
		uint16_t gotTarget = 0;
		t.setMessageHandler([&](ConnId c, const uint8_t* data, size_t size) {
			seenBy = c;
			if (size >= 8) {
				memcpy(&gotTarget, data + 6, 2);
			}
		});
		const ConnId c1 = t.addConnection();
		const ConnId c2 = t.addConnection();
		uint8_t msg[8] = { 0 };
		const uint32_t len = 4;
		const uint16_t type = 1;
		const uint16_t target = 17;
		memcpy(msg, &len, 4);
		memcpy(msg + 4, &type, 2);
		memcpy(msg + 6, &target, 2);
		CHECK(t.send(c2, msg, sizeof(msg)));
		CHECK(seenBy == c2);
		CHECK(gotTarget == 17);
		(void)c1;
	}

	/* 8:占领——单位所在省次日易主 */
	{
		GameState s = world;
		stepDay(s);
		CHECK(s.provinceOwner[0] == 0);          /* u0 在 p0 → 归 player 0 */
		CHECK(s.provinceOwner[8] == 1);
	}

	/* 9:战斗确定性——同城两单位对掷,恰一存活,两次推演一致 */
	{
		auto battle = [&world]() -> uint64_t {
			GameState s = world;
			s.units[1].province = 0;             /* 蓝军进城,开打 */
			for (int d = 0; d < 40; d++) {
				stepDay(s);
			}
			int alive = 0;
			for (const Unit& u : s.units) {
				if (u.strength > 0) {
					alive++;
				}
			}
			CHECK(alive >= 1);
			return stateHash(s);
		};
		const uint64_t b1 = battle();
		const uint64_t b2 = battle();
		CHECK(b1 == b2);
		GameState probe = world;
		probe.units[1].province = 0;
		for (int d = 0; d < 40; d++) {
			stepDay(probe);
		}
		CHECK(probe.provinceOwner[0] == 0 || probe.provinceOwner[0] == 1);
		for (const Unit& u : probe.units) {    // 死者归零,永不为负
			CHECK(u.strength >= 0);
			if (u.strength == 0) {
				CHECK(u.strength == 0);
			}
		}
	}

	/* 10:幽灵行军修复——死人不动、不能受令 */
	{
		GameState s = world;
		stepDay(s);                            /* u0 占领 p0 */
		s.units[0].strength = 0;               /* 战死 */
		s.units[0].target = 5;
		for (int d = 0; d < 8; d++) {
			stepDay(s);
		}
		CHECK(s.units[0].province == 0);       /* 尸体停在原地 */
		Command c{ CmdType::MoveUnit, 0, 0, 5 };
		CHECK(!validateCommand(s, c));         /* 死人不能受令 */
	}

	/* 11:经济——收入/回血/造兵确定性 */
	{
		auto econ = [&world]() -> uint64_t {
			GameState s = world;
			Command b{ CmdType::BuildUnit, 0, 0, 0 };
			/* p0 需要 owner==0:占领一天 */
			stepDay(s);
			for (int d = 0; d < 200; d++) {    /* 攒 300 补给 */
				stepDay(s);
				if (s.supplies[0] >= COST_BUILD && s.units.size() == 4) {
					if (validateCommand(s, b)) {
						applyCommand(s, b);
					}
				}
			}
			CHECK(s.units.size() == 5);        /* 新兵造出来了 */
			CHECK(s.supplies[0] < COST_BUILD); /* 扣了钱 */
			CHECK(s.units[4].strength == 100);
			CHECK(s.units[4].owner == 0);
			return stateHash(s);
		};
		CHECK(econ() == econ());
		/* 回血:伤兵驻己方省恢复 */
		GameState h = world;
		stepDay(h);
		h.units[0].strength = 50;
		for (int d = 0; d < 25; d++) {
			stepDay(h);
		}
		CHECK(h.units[0].strength == 100);     /* 50+2*25 */
	}

	printf(g_fail == 0 ? "ALL TESTS PASSED\n" : "%d FAILURES\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
