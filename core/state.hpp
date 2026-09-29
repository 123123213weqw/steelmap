/* 游戏状态。规则:M0 一切从简,结构只放tick必需的字段;
 * 内容型数据(省份/兵种/国家定义)M1 起走外置数据文件。
 */
#pragma once
#include <cstdint>
#include <vector>

namespace gs {

using ProvinceId = uint16_t;
using UnitId     = uint16_t;
using PlayerId   = uint16_t;

constexpr ProvinceId INVALID_PROVINCE = 0xFFFF;
constexpr PlayerId   SERVER_PLAYER    = 0xFFFF;
constexpr PlayerId   NEUTRAL_PLAYER   = 0xFFFE;   // 省份无主

/* 地形:影响防御减伤 */
enum Terrain : uint8_t {
	TERRAIN_PLAINS = 0,                      // 平原
	TERRAIN_FOREST = 1,                      // 森林 -15% 受伤
	TERRAIN_MOUNTAIN = 2,                    // 山地 -30% 受伤
};

/* 坐标:定点 int32,1 单位 = 1000 毫单位(milli)。
 * 地图整体在 ±1000000 内,保持仿真整数纪律。
 */
struct ProvinceGraph {
	uint16_t count = 0;
	std::vector<std::vector<ProvinceId>> adj;   // 每省邻接列表,升序
	std::vector<int32_t> centerX;               // 单位锚点/标签位置
	std::vector<int32_t> centerY;
	std::vector<std::vector<int32_t>> polyX;    // 省界多边形顶点环(闭合)
	std::vector<std::vector<int32_t>> polyY;
	std::vector<uint8_t> terrain;               // Terrain,地图内容
};

struct Unit {
	UnitId     id = 0;
	PlayerId   owner = 0;
	ProvinceId province = 0;                    // 当前所在省
	ProvinceId target = INVALID_PROVINCE;       // 移动目标,INVALID 表示驻留
	int32_t    strength = 100;                  // <=0 视为阵亡,不再清除(保 id 稳定)
	uint8_t    fog = 0;                         // 仅用于按观察者过滤的快照副本
};

struct GameState {
	uint32_t tick = 0;                          // 游戏内天数
	uint8_t  phase = 1;                         // 0=大厅 1=对局中(序列化 v5)
	uint64_t rng[4] = { 0, 0, 0, 0 };
	ProvinceGraph map;
	std::vector<PlayerId> provinceOwner;        // 与 map.count 同长
	uint32_t supplies[16] = { 0 };              // 每玩家补给库存
	std::vector<Unit> units;                    // id == 下标,升序存放

	void buildDefaultMap(uint16_t provinceCount);
	void seedRng(uint64_t seed) ;
};

/* 状态哈希 = 序列化字节流的 FNV-1a,用于确定性校验 */
uint64_t stateHash(const GameState& s);

} // namespace gs
