/* 小端字节读写:core 序列化与 net 协议共用一份实现,避免格式分叉。 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gs {

inline void putU16(std::vector<uint8_t>& out, uint16_t v)
{
	out.push_back((uint8_t)(v & 0xFF));
	out.push_back((uint8_t)(v >> 8));
}

inline void putU32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
	}
}

inline void putU64(std::vector<uint8_t>& out, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
	}
}

struct Cursor {
	const uint8_t* data = nullptr;
	size_t size = 0;
	size_t pos = 0;
	bool ok = true;

	uint16_t u16()
	{
		if (!ok || pos + 2 > size) {
			ok = false;
			return 0;
		}
		uint16_t v = (uint16_t)(data[pos] | (data[pos + 1] << 8));
		pos += 2;
		return v;
	}

	uint32_t u32()
	{
		if (!ok || pos + 4 > size) {
			ok = false;
			return 0;
		}
		uint32_t v = 0;
		for (int i = 0; i < 4; i++) {
			v |= (uint32_t)data[pos + i] << (i * 8);
		}
		pos += 4;
		return v;
	}

	uint64_t u64()
	{
		if (!ok || pos + 8 > size) {
			ok = false;
			return 0;
		}
		uint64_t v = 0;
		for (int i = 0; i < 8; i++) {
			v |= (uint64_t)data[pos + i] << (i * 8);
		}
		pos += 8;
		return v;
	}
};

} // namespace gs
