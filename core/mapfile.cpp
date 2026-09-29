#include "mapfile.hpp"
#include "bytes.hpp"
#include <cstdio>
#include <cstddef>

namespace gs {

namespace {

constexpr uint16_t MAP_VERSION = 2;          /* v2:几何 → 地形 → 邻接 */

void putI32(std::vector<uint8_t>& out, int32_t v)
{
	putU32(out, (uint32_t)v);
}

int32_t getI32(Cursor& c)
{
	return (int32_t)c.u32();
}

} // namespace

bool saveMapFile(const char* path, const GameState& s)
{
	FILE* f = fopen(path, "wb");
	if (f == nullptr) {
		return false;
	}
	std::vector<uint8_t> out;
	out.push_back('M');
	out.push_back('A');
	out.push_back('P');
	putU16(out, MAP_VERSION);
	putU16(out, s.map.count);
	for (uint16_t p = 0; p < s.map.count; p++) {
		putI32(out, s.map.centerX[p]);
		putI32(out, s.map.centerY[p]);
		putU16(out, (uint16_t)s.map.polyX[p].size());
		for (size_t k = 0; k < s.map.polyX[p].size(); k++) {
			putI32(out, s.map.polyX[p][k]);
			putI32(out, s.map.polyY[p][k]);
		}
	}
	for (uint16_t p = 0; p < s.map.count; p++) {
		out.push_back(s.map.terrain.empty() ? (uint8_t)TERRAIN_PLAINS
			: s.map.terrain[p]);
	}
	for (uint16_t p = 0; p < s.map.count; p++) {
		putU16(out, (uint16_t)s.map.adj[p].size());
		for (ProvinceId n : s.map.adj[p]) {
			putU16(out, n);
		}
	}
	const size_t written = fwrite(out.data(), 1, out.size(), f);
	fclose(f);
	return written == out.size();
}

bool loadMapFile(const char* path, GameState& s)
{
	FILE* f = fopen(path, "rb");
	if (f == nullptr) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 64L * 1024L * 1024L) {
		fclose(f);
		return false;
	}
	std::vector<uint8_t> buf((size_t)size);
	if (fread(buf.data(), 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		return false;
	}
	fclose(f);

	if (buf.size() < 7 || buf[0] != 'M' || buf[1] != 'A'
		|| buf[2] != 'P') {
		return false;
	}
	Cursor r{ buf.data() + 3, buf.size() - 3 };   // 跳过 3 字节魔数
	const uint16_t fileVer = r.u16();
	if (fileVer != 1 && fileVer != MAP_VERSION) {
		return false;
	}
	const uint16_t count = r.u16();
	if (!r.ok || count == 0) {
		return false;
	}

	ProvinceGraph m;
	m.count = count;
	m.centerX.resize(count);
	m.centerY.resize(count);
	m.polyX.resize(count);
	m.polyY.resize(count);
	m.adj.resize(count);
	for (uint16_t p = 0; p < count; p++) {
		m.centerX[p] = getI32(r);
		m.centerY[p] = getI32(r);
		const uint16_t n = r.u16();
		if (!r.ok || n < 3) {
			return false;
		}
		for (uint16_t k = 0; k < n; k++) {
			m.polyX[p].push_back(getI32(r));
			m.polyY[p].push_back(getI32(r));
		}
	}
	m.terrain.assign(count, TERRAIN_PLAINS);
	if (fileVer >= 2) {
		for (uint16_t p = 0; p < count; p++) {
			m.terrain[p] = buf[r.pos];
			r.pos += 1;
			if (!r.ok) {
				return false;
			}
		}
	}
	for (uint16_t p = 0; p < count; p++) {
		const uint16_t deg = r.u16();
		if (!r.ok) {
			return false;
		}
		for (uint16_t k = 0; k < deg; k++) {
			const ProvinceId n = r.u16();
			if (n >= count) {
				return false;           // 坏邻接:越界
			}
			m.adj[p].push_back(n);
		}
	}
	if (!r.ok) {
		return false;
	}
	s.map = std::move(m);
	/* 归属是对局状态不是地图内容:加载后重置为全中立,并按新省数定长 */
	s.provinceOwner.assign(s.map.count, NEUTRAL_PLAYER);
	return true;
}

} // namespace gs
