/* 应用层消息:[u16 消息类型][载荷]。
 * C→S: Join(选座) / Command(玩家命令)
 * S→C: Accept(入座确认) / Snapshot(全量状态)
 * 帧长字段由 frame.hpp 处理,这里只管消息内部。
 */
#pragma once
#include "../core/bytes.hpp"
#include "../core/command.hpp"
#include <vector>

namespace gs {

enum class MsgType : uint16_t {
	Join     = 1,   // C→S [u16 seat] seat=0xFFFF 表示任意空位
	Accept   = 2,   // S→C [u16 playerId]
	Command  = 3,   // C→S [u16 cmdType][u16 unit][u16 target]
	Snapshot = 4,   // S→C [GameState 序列化字节]
	Event    = 5,   // S→C [u16 evType][u16 player][u16 a][u16 b]
};

/* 服务器由每日状态差异生成的事件(客户端展示用,不进仿真) */
enum class EventType : uint16_t {
	Capture = 1,    // a=省 b=旧主     player=新主
	Died    = 2,    // a=单位 b=兵力   player=所属主
	Built   = 3,    // a=单位          player=所属主
	Winner  = 4,    //                 player=胜者
};

struct Msg {
	MsgType type = MsgType::Join;
	uint16_t seat = 0xFFFF;      // Join
	uint16_t player = 0;         // Accept
	Command cmd;                 // Command(player 字段由服务器覆盖,发送时可填 0)
	std::vector<uint8_t> state;  // Snapshot
	uint16_t evType = 0;         // Event
	uint16_t evPlayer = 0;
	uint16_t evA = 0;
	uint16_t evB = 0;
};

inline void encodeJoin(std::vector<uint8_t>& out, uint16_t seat)
{
	putU16(out, (uint16_t)MsgType::Join);
	putU16(out, seat);
}

inline void encodeAccept(std::vector<uint8_t>& out, uint16_t player)
{
	putU16(out, (uint16_t)MsgType::Accept);
	putU16(out, player);
}

inline void encodeCommand(std::vector<uint8_t>& out, const Command& c)
{
	putU16(out, (uint16_t)MsgType::Command);
	putU16(out, (uint16_t)c.type);
	putU16(out, c.unit);
	putU16(out, c.target);
}

inline void encodeEvent(std::vector<uint8_t>& out, uint16_t evType,
	uint16_t player, uint16_t a, uint16_t b)
{
	putU16(out, (uint16_t)MsgType::Event);
	putU16(out, evType);
	putU16(out, player);
	putU16(out, a);
	putU16(out, b);
}

inline void encodeSnapshot(std::vector<uint8_t>& out,
	const std::vector<uint8_t>& stateBytes)
{
	putU16(out, (uint16_t)MsgType::Snapshot);
	out.insert(out.end(), stateBytes.begin(), stateBytes.end());
}

inline bool decodeMsg(const uint8_t* data, size_t size, Msg& m)
{
	Cursor c{ data, size };
	const uint16_t type = c.u16();
	if (!c.ok) {
		return false;
	}
	m = Msg{};
	m.type = (MsgType)type;
	switch (m.type) {
	case MsgType::Join:
		m.seat = c.u16();
		return c.ok;
	case MsgType::Accept:
		m.player = c.u16();
		return c.ok;
	case MsgType::Command: {
		const uint16_t ct = c.u16();
		m.cmd.unit = c.u16();
		m.cmd.target = c.u16();
		m.cmd.type = (CmdType)ct;
		m.cmd.player = 0;
		return c.ok;
	}
	case MsgType::Snapshot:
		m.state.assign(data + c.pos, data + size);
		return true;
	case MsgType::Event:
		m.evType = c.u16();
		m.evPlayer = c.u16();
		m.evA = c.u16();
		m.evB = c.u16();
		return c.ok;
	default:
		return false;
	}
}

} // namespace gs
