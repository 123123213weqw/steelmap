/* 序列化 = 存档 = 网络 = 回放,一份代码三处用。
 * 格式:小端、定长头(magic + version)、字段顺序固定。
 * 改格式必须升 version 并写迁移,否则旧回放/存档全部作废。
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "state.hpp"

namespace gs {

void serialize(const GameState& s, std::vector<uint8_t>& out);
bool deserialize(const uint8_t* data, size_t size, GameState& out);

} // namespace gs
