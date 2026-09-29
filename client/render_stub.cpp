/* 无渲染构建的占位:--render 时提示并退出 */
#include "render.hpp"
#include <cstdio>

namespace gs {

int runRenderWindow(SharedView&, int,
	const std::function<void(uint16_t, uint16_t, uint16_t)>&,
	const std::function<void(uint16_t)>&,
	const char*, uint32_t)
{
	printf("[render] not built (STEELMAP_RENDER=OFF)\n");
	return 1;
}

} // namespace gs
