#include "command.hpp"
#include <cstddef>

namespace gs {

bool validateCommand(const GameState& s, const Command& c)
{
	switch (c.type) {
	case CmdType::MoveUnit:
		return c.unit < s.units.size()
			&& c.target < s.map.count
			&& s.units[c.unit].owner == c.player
			&& s.units[c.unit].strength > 0;       // 幽灵行军修复
	case CmdType::BuildUnit:
		return c.target < s.map.count
			&& c.player < 16
			&& s.provinceOwner[c.target] == c.player
			&& s.supplies[c.player] >= COST_BUILD
			&& s.units.size() < 1024;
	case CmdType::SetPlayerReady:
		return true;
	case CmdType::SetSpeed:
		return c.player == 0 && c.target <= 5000;   /* 房主控速 */
	case CmdType::StartGame:
		return c.player == 0 && s.phase == 0;       /* 房主在大厅启动 */
	default:
		return false;
	}
}

void applyCommand(GameState& s, const Command& c)
{
	if (!validateCommand(s, c)) {
		return;                                 // 服务器边界强制校验,误用即无操作
	}
	switch (c.type) {
	case CmdType::MoveUnit: {
		Unit& u = s.units[c.unit];
		u.target = (u.province == c.target) ? INVALID_PROVINCE : c.target;
		break;
	}
	case CmdType::BuildUnit:
		if (c.player < 16 && s.supplies[c.player] >= COST_BUILD) {
			s.supplies[c.player] -= COST_BUILD;
			Unit u;
			u.id = (UnitId)s.units.size();
			u.owner = c.player;
			u.province = c.target;
			u.strength = UNIT_MAX_STRENGTH;
			s.units.push_back(u);
		}
		break;
	case CmdType::StartGame:
		if (s.phase == 0) {
			s.phase = 1;
		}
		break;
	case CmdType::SetPlayerReady:
		break;
	default:
		break;
	}
}

} // namespace gs
