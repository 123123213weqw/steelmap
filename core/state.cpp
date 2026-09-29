#include "state.hpp"
#include "rng.hpp"
#include "serialize.hpp"
#include <cstddef>
#include <cmath>

namespace gs {

void GameState::buildDefaultMap(uint16_t provinceCount)
{
	map.count = provinceCount;
	map.adj.assign(provinceCount, {});
	map.centerX.assign(provinceCount, 0);
	map.centerY.assign(provinceCount, 0);
	map.polyX.assign(provinceCount, {});
	map.polyY.assign(provinceCount, {});
	map.terrain.assign(provinceCount, TERRAIN_PLAINS);
	provinceOwner.assign(provinceCount, NEUTRAL_PLAYER);
	/* 占位地图:环 + 对角弦,每省一个正八边形(半径 420,环半径 3200) */
	const double PI = 3.14159265358979323846;
	for (uint16_t i = 0; i < provinceCount; i++) {
		const double angle = (double)i * (2.0 * PI / provinceCount)
			- PI / 2.0;
		map.centerX[i] = (int32_t)(cos(angle) * 3200.0);
		map.centerY[i] = (int32_t)(sin(angle) * 3200.0);
		for (int k = 0; k < 8; k++) {
			const double a = (double)k * (2.0 * PI / 8.0);
			map.polyX[i].push_back((int32_t)(cos(angle) * 3200.0
				+ cos(a) * 420.0));
			map.polyY[i].push_back((int32_t)(sin(angle) * 3200.0
				+ sin(a) * 420.0));
		}
		const uint16_t prev =
			(uint16_t)((i + provinceCount - 1) % provinceCount);
		const uint16_t next = (uint16_t)((i + 1) % provinceCount);
		const uint16_t opposite =
			(uint16_t)((i + provinceCount / 2) % provinceCount);
		for (ProvinceId n : { prev, next, opposite }) {
			if (n != i) {
				map.adj[i].push_back(n);
			}
		}
	}
	for (auto& list : map.adj) {
		bool swapped = true;
		while (swapped) {
			swapped = false;
			for (size_t k = 1; k < list.size(); k++) {
				if (list[k - 1] == list[k]) {
					list.erase(list.begin() + (long)k);
					k--;
					continue;
				}
				if (list[k - 1] > list[k]) {
					ProvinceId t = list[k - 1];
					list[k - 1] = list[k];
					list[k] = t;
					swapped = true;
				}
			}
		}
	}
}

void GameState::seedRng(uint64_t seed)
{
	rngSeed(rng, seed);
}

uint64_t stateHash(const GameState& s)
{
	std::vector<uint8_t> bytes;
	serialize(s, bytes);
	uint64_t h = 1469598103934665603ULL;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ULL;
	}
	return h;
}

} // namespace gs
