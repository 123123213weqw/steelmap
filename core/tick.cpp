#include "tick.hpp"
#include "command.hpp"
#include "rng.hpp"
#include <queue>
#include <cstddef>

namespace gs {

std::vector<uint32_t> distanceFromSet(const ProvinceGraph& map,
	const std::vector<ProvinceId>& sources)
{
	constexpr uint32_t UNREACHABLE = 0xFFFFFFFFu;
	std::vector<uint32_t> dist(map.count, UNREACHABLE);
	std::queue<ProvinceId> q;
	for (ProvinceId src : sources) {
		if (src < map.count && dist[src] == UNREACHABLE) {
			dist[src] = 0;
			q.push(src);
		}
	}
	while (!q.empty()) {
		ProvinceId cur = q.front();
		q.pop();
		for (ProvinceId n : map.adj[cur]) {
			if (dist[n] == UNREACHABLE) {
				dist[n] = dist[cur] + 1;
				q.push(n);
			}
		}
	}
	return dist;
}

std::vector<uint32_t> distanceFrom(const ProvinceGraph& map, ProvinceId dest)
{
	return distanceFromSet(map, { dest });
}

/* 每日战斗:同城异主单位互相造成 8-15 点伤害,目标随机(整数 RNG,随状态走) */
static void resolveCombat(GameState& s)
{
	for (ProvinceId p = 0; p < s.map.count; p++) {
		bool contested = false;
		PlayerId first = SERVER_PLAYER;
		for (const Unit& u : s.units) {
			if (u.province == p && u.strength > 0) {
				if (first == SERVER_PLAYER) {
					first = u.owner;
				} else if (u.owner != first) {
					contested = true;
					break;
				}
			}
		}
		if (!contested) {
			continue;
		}
		for (size_t i = 0; i < s.units.size(); i++) {
			Unit& u = s.units[i];
			if (u.province != p || u.strength <= 0) {
				continue;
			}
			/* 选一个还活着的敌人(id 最小者受随机偏置,确定性) */
			std::vector<size_t> foes;
			for (size_t j = 0; j < s.units.size(); j++) {
				const Unit& e = s.units[j];
				if (e.province == p && e.strength > 0
					&& e.owner != u.owner) {
					foes.push_back(j);
				}
			}
			if (foes.empty()) {
				continue;
			}
			const uint64_t roll = rngNext(s.rng);
			Unit& victim = s.units[foes[roll % foes.size()]];
			/* 地形减伤:守方在山地/森林少挨打 */
			int32_t dmg = (int32_t)(8 + roll % 8);
			const uint8_t t =
				(p < s.map.terrain.size()) ? s.map.terrain[p]
					: (uint8_t)TERRAIN_PLAINS;
			if (t == TERRAIN_MOUNTAIN) {
				dmg = dmg * 7 / 10;
			} else if (t == TERRAIN_FOREST) {
				dmg = dmg * 85 / 100;
			}
			victim.strength -= dmg;
			if (victim.strength < 0) {
				victim.strength = 0;         // 阵亡即归零,不留负数
			}
		}
	}
}

/* 劣势撤退:敌众我寡(2:1 以上)且残血(<40)→ 退往无敌人的邻省
 * (优先己方省,再取 id 最小),无处可退则死战。确定性。
 */
static void resolveRetreat(GameState& s)
{
	for (ProvinceId p = 0; p < s.map.count; p++) {
		for (size_t i = 0; i < s.units.size(); i++) {
			Unit& u = s.units[i];
			if (u.province != p || u.strength <= 0
				|| u.strength >= 40) {
				continue;
			}
			int allies = 0, enemies = 0;
			for (const Unit& e : s.units) {
				if (e.province != p || e.strength <= 0) {
					continue;
				}
				if (e.owner == u.owner) {
					allies++;
				} else {
					enemies++;
				}
			}
			if (enemies < 2 * allies) {
				continue;
			}
			ProvinceId retreat = INVALID_PROVINCE;
			for (ProvinceId n : s.map.adj[p]) {
				bool safe = true;
				for (const Unit& e : s.units) {
					if (e.province == n && e.strength > 0
						&& e.owner != u.owner) {
						safe = false;
						break;
					}
				}
				if (!safe) {
					continue;
				}
				if (retreat == INVALID_PROVINCE) {
					retreat = n;
				}
				if (n < s.map.count
					&& n < s.provinceOwner.size()
					&& s.provinceOwner[n] == u.owner) {
					retreat = n;              /* 优先己方省 */
					break;
				}
			}
			if (retreat != INVALID_PROVINCE) {
				u.province = retreat;
				u.target = INVALID_PROVINCE;
			}
		}
	}
}

/* 占领:一省只剩同一主的单位且与省主不同 → 易主 */
static void resolveCapture(GameState& s)
{
	for (ProvinceId p = 0; p < s.map.count; p++) {
		PlayerId only = SERVER_PLAYER;
		bool mixed = false;
		for (const Unit& u : s.units) {
			if (u.province == p && u.strength > 0) {
				if (only == SERVER_PLAYER) {
					only = u.owner;
				} else if (u.owner != only) {
					mixed = true;
					break;
				}
			}
		}
		if (!mixed && only != SERVER_PLAYER
			&& s.provinceOwner[p] != only) {
			s.provinceOwner[p] = only;
		}
	}
}

void stepDay(GameState& s)
{
	s.tick++;
	for (Unit& u : s.units) {
		if (u.strength <= 0) {                  // 幽灵行军修复
			continue;
		}
		if (u.target == INVALID_PROVINCE || u.target >= s.map.count) {
			continue;
		}
		if (u.province == u.target) {
			u.target = INVALID_PROVINCE;
			continue;
		}
		/* 到目标的距离场,取距离减一的邻居里 id 最小的 → 下一跳确定 */
		const std::vector<uint32_t> dist = distanceFrom(s.map, u.target);
		const uint32_t here = dist[u.province];
		if (here == 0xFFFFFFFFu) {
			u.target = INVALID_PROVINCE;   // 不连通(不应发生),取消命令
			continue;
		}
		ProvinceId next = INVALID_PROVINCE;
		for (ProvinceId n : s.map.adj[u.province]) {
			if (dist[n] == here - 1
				&& (next == INVALID_PROVINCE || n < next)) {
				next = n;
			}
		}
		if (next != INVALID_PROVINCE) {
			u.province = next;
			if (next == u.target) {
				u.target = INVALID_PROVINCE;
			}
		}
	}
	resolveCombat(s);
	resolveRetreat(s);
	resolveCapture(s);
	/* 经济:每省每日收入 + 驻己方省回血 */
	for (uint16_t p = 0; p < s.map.count; p++) {
		const PlayerId o = s.provinceOwner[p];
		if (o < 16) {
			s.supplies[o] += INCOME_PER_PROVINCE;
		}
	}
	for (Unit& u : s.units) {
		if (u.strength > 0 && u.province < s.map.count
			&& s.provinceOwner[u.province] == u.owner
			&& u.strength < UNIT_MAX_STRENGTH) {
			u.strength += HEAL_AT_HOME;
			if (u.strength > UNIT_MAX_STRENGTH) {
				u.strength = UNIT_MAX_STRENGTH;
			}
		}
	}
}

} // namespace gs
