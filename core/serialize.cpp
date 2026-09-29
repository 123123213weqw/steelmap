#include "serialize.hpp"
#include "bytes.hpp"
#include <cstddef>

namespace gs {

namespace {

constexpr uint8_t MAGIC[2] = { 'G', 'S' };
constexpr uint16_t VERSION = 5;

} // namespace

void serialize(const GameState& s, std::vector<uint8_t>& out)
{
	out.push_back(MAGIC[0]);
	out.push_back(MAGIC[1]);
	putU16(out, VERSION);
	putU32(out, s.tick);
	out.push_back(s.phase);
	out.push_back(0);
	out.push_back(0);
	out.push_back(0);
	for (int i = 0; i < 4; i++) {
		putU64(out, s.rng[i]);
	}
	putU16(out, s.map.count);
	for (uint16_t p = 0; p < s.map.count; p++) {
		putU16(out, (uint16_t)s.map.adj[p].size());
		for (ProvinceId n : s.map.adj[p]) {
			putU16(out, n);
		}
	}
	for (uint16_t p = 0; p < s.map.count; p++) {
		out.push_back(s.map.terrain.empty() ? (uint8_t)TERRAIN_PLAINS
			: s.map.terrain[p]);
	}
	for (uint16_t p = 0; p < s.map.count; p++) {
		putU32(out, (uint32_t)s.map.centerX[p]);
		putU32(out, (uint32_t)s.map.centerY[p]);
	}
	for (uint16_t p = 0; p < s.map.count; p++) {
		putU16(out, (uint16_t)s.map.polyX[p].size());
		for (size_t k = 0; k < s.map.polyX[p].size(); k++) {
			putU32(out, (uint32_t)s.map.polyX[p][k]);
			putU32(out, (uint32_t)s.map.polyY[p][k]);
		}
	}
	for (uint16_t p = 0; p < s.map.count; p++) {
		putU16(out, s.provinceOwner.empty() ? (uint16_t)NEUTRAL_PLAYER
			: (uint16_t)s.provinceOwner[p]);
	}
	for (int i = 0; i < 16; i++) {
		putU32(out, s.supplies[i]);
	}
	putU32(out, (uint32_t)s.units.size());
	for (const Unit& u : s.units) {
		putU16(out, u.id);
		putU16(out, u.owner);
		putU16(out, u.province);
		putU16(out, u.target);
		putU32(out, (uint32_t)u.strength);
		out.push_back(u.fog);
		out.push_back(0);
		out.push_back(0);
		out.push_back(0);
	}
}

bool deserialize(const uint8_t* data, size_t size, GameState& out)
{
	Cursor r{ data, size };
	if (r.u16() != 0x5347) {                      // "GS" 小端
		return false;
	}
	if (r.u16() != VERSION) {
		return false;
	}
	GameState s;
	s.tick = r.u32();
	s.phase = data[r.pos]; r.pos += 4;             /* phase + 3 填充 */
	if (!r.ok) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		s.rng[i] = r.u64();
	}
	s.map.count = r.u16();
	if (!r.ok || s.map.count == 0) {
		return false;
	}
	s.map.adj.resize(s.map.count);
	for (uint16_t p = 0; p < s.map.count; p++) {
		const uint16_t deg = r.u16();
		if (!r.ok) {
			return false;
		}
		s.map.adj[p].clear();
		for (uint16_t k = 0; k < deg; k++) {
			s.map.adj[p].push_back(r.u16());
		}
	}
	s.map.terrain.assign(s.map.count, TERRAIN_PLAINS);
	for (uint16_t p = 0; p < s.map.count; p++) {
		s.map.terrain[p] = data[r.pos];
		r.pos += 1;
	}
	if (!r.ok) {
		return false;
	}
	s.map.centerX.resize(s.map.count);
	s.map.centerY.resize(s.map.count);
	for (uint16_t p = 0; p < s.map.count; p++) {
		s.map.centerX[p] = (int32_t)r.u32();
		s.map.centerY[p] = (int32_t)r.u32();
	}
	s.map.polyX.resize(s.map.count);
	s.map.polyY.resize(s.map.count);
	for (uint16_t p = 0; p < s.map.count; p++) {
		const uint16_t n = r.u16();
		if (!r.ok) {
			return false;
		}
		s.map.polyX[p].clear();
		s.map.polyY[p].clear();
		for (uint16_t k = 0; k < n; k++) {
			s.map.polyX[p].push_back((int32_t)r.u32());
			s.map.polyY[p].push_back((int32_t)r.u32());
		}
	}
	s.provinceOwner.resize(s.map.count);
	for (uint16_t p = 0; p < s.map.count; p++) {
		const uint16_t o = r.u16();
		s.provinceOwner[p] = (o == (uint16_t)NEUTRAL_PLAYER)
			? NEUTRAL_PLAYER : (PlayerId)o;
	}
	for (int i = 0; i < 16; i++) {
		s.supplies[i] = r.u32();
	}
	const uint32_t unitCount = r.u32();
	if (!r.ok || unitCount > 4096) {
		return false;
	}
	s.units.resize(unitCount);
	for (uint32_t i = 0; i < unitCount; i++) {
		s.units[i].id = r.u16();
		s.units[i].owner = r.u16();
		s.units[i].province = r.u16();
		s.units[i].target = r.u16();
		s.units[i].strength = (int32_t)r.u32();
		s.units[i].fog = data[r.pos];
		r.pos += 4;
	}
	if (!r.ok) {
		return false;
	}
	out = std::move(s);
	return true;
}

} // namespace gs
