/* 仿真推进:stepDay = 服务器一个 tick。
 * 顺序固定:先应用当日命令(由调用方排好序),再按单位 id 升序结算移动。
 */
#pragma once
#include "state.hpp"

namespace gs {

/* 每个单位向自己的 target 沿最短路前进一跳;到达后清除目标。 */
void stepDay(GameState& s);

/* BFS 求各省到 dest 的跳数,用于确定下一跳。 */
std::vector<uint32_t> distanceFrom(const ProvinceGraph& map, ProvinceId dest);

/* 多源 BFS:到最近源点的跳数(AI 找最近目标用)。 */
std::vector<uint32_t> distanceFromSet(const ProvinceGraph& map,
	const std::vector<ProvinceId>& sources);

} // namespace gs
