/* 玩家命令:客户端只能产生这些意图,服务器逐一校验后应用。
 * 命令流 + 种子 = 完整回放,所以命令必须可序列化、可复放。
 */
#pragma once
#include "state.hpp"

namespace gs {

enum class CmdType : uint16_t {
	None = 0,
	MoveUnit = 1,      // 让 unit 前往 target 省(可改目标)
	SetPlayerReady = 2, // 联机大厅用,M0 预留
	BuildUnit = 3,     // 花 COST_BUILD 补给在 target 己方省造一个新单位
	SetSpeed = 4,     // target = 毫秒/天(0=暂停);墙钟节奏,不进回放
	StartGame = 5,    // 房主在大厅启动对局:phase 0→1(进回放)
};

constexpr uint32_t COST_BUILD = 300;          // 造兵补给
constexpr uint32_t INCOME_PER_PROVINCE = 2;   // 每省每日收入
constexpr int32_t  HEAL_AT_HOME = 2;          // 驻己方省每日回血
constexpr int32_t  UNIT_MAX_STRENGTH = 100;

struct Command {
	CmdType     type = CmdType::None;
	PlayerId    player = 0;              // 命令归属,服务器按连接填入并校验
	UnitId      unit = 0;                // BuildUnit 时无意义,填 0
	ProvinceId  target = INVALID_PROVINCE;
};

/* 校验命令合法性与归属。返回 false 时状态必须保持不变。 */
bool validateCommand(const GameState& s, const Command& c);

/* 应用命令。仅在 validate 通过后调用。 */
void applyCommand(GameState& s, const Command& c);

} // namespace gs
