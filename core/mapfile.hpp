/* 地图内容文件:与存档(状态序列化)分开——地图是内容,状态是对局。
 * 服务器加载后地图进 GameState,随快照下发,客户端无需地图文件。
 *
 * 格式(小端):['M','A']['P'][u16 version=1][u16 provinceCount]
 *   每省:[i32 cx][i32 cy][u16 顶点数][顶点 (i32,i32)*n]
 *        [u16 邻接数][邻接 u16*n]
 * 坐标单位:milli(1 单位 = 1000)。
 */
#pragma once
#include "state.hpp"

namespace gs {

bool saveMapFile(const char* path, const GameState& s);
bool loadMapFile(const char* path, GameState& s);   // 只替换 s.map

} // namespace gs
