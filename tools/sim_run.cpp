/* headless 仿真运行器:M0 的"服务器核心循环"雏形 + 回放工具。
 * 用法: sim_run --seed 42 --days 60 --script 路径
 * 脚本格式(每行): tick unitId targetProvince
 * player 字段取单位归属(服务器场景下由连接决定,这里自举)。
 */
#include "../core/command.hpp"
#include "../core/mapfile.hpp"
#include "../core/serialize.hpp"
#include "../core/tick.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace gs;

int main(int argc, char** argv)
{
	uint64_t seed = 42;
	uint32_t days = 60;
	const char* scriptPath = nullptr;
	const char* mapPath = nullptr;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			seed = strtoull(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--days") && i + 1 < argc) {
			days = (uint32_t)strtoul(argv[++i], nullptr, 10);
		} else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
			scriptPath = argv[++i];
		} else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
			mapPath = argv[++i];
		}
	}

	GameState s;
	if (mapPath != nullptr) {
		if (!loadMapFile(mapPath, s)) {
			printf("cannot load map: %s\n", mapPath);
			return 1;
		}
	} else {
		s.buildDefaultMap(32);
	}
	s.seedRng(seed);
	for (UnitId i = 0; i < 4; i++) {
		Unit u;
		u.id = i;
		u.owner = (PlayerId)i;
		u.province = (ProvinceId)((i * (int)s.map.count / 4)
			% (int)s.map.count);
		s.units.push_back(u);
	}

	struct Scheduled {
		uint32_t tick;
		Command cmd;
	};
	std::vector<Scheduled> script;
	if (scriptPath != nullptr) {
		FILE* f = fopen(scriptPath, "r");
		if (f == nullptr) {
			printf("cannot open script: %s\n", scriptPath);
			return 1;
		}
		char line[128];
		while (fgets(line, sizeof(line), f) != nullptr) {
			unsigned t, a, b;
			char tag;
			Command c;
			if (sscanf(line, "%u B %u", &t, &a) == 2) {
				c.type = CmdType::BuildUnit;
				c.target = (ProvinceId)a;
			} else if (sscanf(line, "%u G %u", &t, &a) == 2) {
				c.type = CmdType::StartGame;   /* 房主开局令 */
				c.player = 0;
			} else if (sscanf(line, "%u %u %u", &t, &a, &b) == 3) {
				c.type = CmdType::MoveUnit;
				c.unit = (UnitId)a;
				c.target = (ProvinceId)b;
				if (c.unit < s.units.size()) {
					c.player = s.units[c.unit].owner;
				}
			} else {
				continue;
			}
			script.push_back({ t, c });
		}
		fclose(f);
	}

	for (uint32_t d = 0; d < days; d++) {
		for (const Scheduled& sc : script) {
			Command cmd = sc.cmd;
			if (cmd.type == CmdType::BuildUnit
				&& cmd.target < s.map.count) {
				cmd.player = s.provinceOwner[cmd.target];
			}
			if (sc.tick == s.tick && validateCommand(s, cmd)) {
				applyCommand(s, cmd);
			}
		}
		stepDay(s);
		if (s.tick % 10 == 0) {
			printf("day %3u:", s.tick);
			for (const Unit& u : s.units) {
				printf("  u%u@p%u%s", u.id, u.province,
					u.target == INVALID_PROVINCE ? "" : "->move");
			}
			printf("\n");
		}
	}

	printf("final hash: %016llx\n", (unsigned long long)stateHash(s));
	return 0;
}
