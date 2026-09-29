/* 渲染模式:网络线程把快照写进 SharedView,渲染线程取副本绘制。
 * 线程边界清晰:SDL/ImGui 只活在渲染线程(XRT worker 之外)。
 */
#pragma once
#include "../core/state.hpp"
#include <functional>
#include <mutex>

namespace gs {

struct EventLine {
	uint32_t day = 0;
	uint16_t type = 0;        // EventType
	uint16_t player = 0;
	uint16_t a = 0, b = 0;
};

struct SharedView {
	std::mutex mu;
	GameState state;
	uint64_t hash = 0;
	bool has = false;
	std::vector<EventLine> events;   // 最近事件,上限 60 条
	uint64_t snapSeq = 0;            // 每个快照 +1,大厅里 tick 不涨也算活
};

/* 阻塞运行窗口,直到关闭/ESC/截图完成。
 * sendCommand 在渲染线程被调用,调用方负责转到 worker 线程发送。
 * shotPath 非空时:tick 达到 shotAtTick 后抓帧存 BMP 并退出。
 * 返回 0 正常。
 */
/* sendCommand(cmdType, unit, target) 与 sendJoin(seat) 由调用方转到
 * worker 线程发送 */
int runRenderWindow(SharedView& view, int myPlayer,
	const std::function<void(uint16_t cmdType, uint16_t unit,
		uint16_t target)>& sendCommand,
	const std::function<void(uint16_t seat)>& sendJoin,
	const char* shotPath, uint32_t shotAtTick);

} // namespace gs
