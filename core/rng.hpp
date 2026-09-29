/* 仿真内唯一随机源:xoshiro256**,整数运算。
 * 状态存在 GameState 里随存档序列化,保证回放/联机一致。
 */
#pragma once
#include <cstdint>

namespace gs {

inline uint64_t splitmix64(uint64_t& x)
{
	uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

inline void rngSeed(uint64_t rng[4], uint64_t seed)
{
	uint64_t s = seed;
	rng[0] = splitmix64(s);
	rng[1] = splitmix64(s);
	rng[2] = splitmix64(s);
	rng[3] = splitmix64(s);
}

inline uint64_t rngNext(uint64_t rng[4])
{
	const uint64_t r = rng[1] * 5ULL;
	const uint64_t result = ((r << 7) | (r >> 57)) * 9ULL;
	const uint64_t t = rng[1] << 17;
	rng[2] ^= rng[0];
	rng[3] ^= rng[1];
	rng[1] ^= rng[2];
	rng[0] ^= rng[3];
	rng[2] ^= t;
	rng[3] = (rng[3] << 45) | (rng[3] >> 19);
	return result;
}

} // namespace gs
