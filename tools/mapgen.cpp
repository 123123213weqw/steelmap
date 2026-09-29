/* Voronoi 地图生成器:抖动网格撒点 → 半平面切割求胞腔 → 共享边判定邻接
 * 输出地图内容文件(core/mapfile 格式)。确定性:同 seed 同图。
 * 用法: mapgen --out maps/xx.map --grid 8 --seed 7
 */
#include "../core/mapfile.hpp"
#include "../core/rng.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace gs;

namespace {

struct Pt {
	int64_t x, y;
};

/* 有符号面积×2:AB×AC */
int64_t cross(Pt a, Pt b, Pt c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/* Sutherland-Hodgman:保留 2(Q-S)·X <= |Q|²-|S|² 一侧(靠近 S 的半平面) */
std::vector<Pt> clipByBisector(std::vector<Pt> poly, Pt s, Pt q)
{
	const int64_t a = 2 * (q.x - s.x);
	const int64_t b = 2 * (q.y - s.y);
	const int64_t c = q.x * q.x + q.y * q.y - s.x * s.x - s.y * s.y;
	/* 判定: a*x + b*y <= c  (即 |X-S|² <= |X-Q|²) */
	auto side = [&](Pt p) -> int64_t { return a * p.x + b * p.y - c; };
	std::vector<Pt> out;
	const size_t n = poly.size();
	for (size_t i = 0; i < n; i++) {
		Pt cur = poly[i];
		Pt prev = poly[(i + n - 1) % n];
		const int64_t sc = side(cur);
		const int64_t sp = side(prev);
		if (sc <= 0) {
			if (sp > 0) {
				/* 交点入界 */
				const int64_t d = sp - sc;
				out.push_back({ prev.x + (cur.x - prev.x) * sp / d,
					prev.y + (cur.y - prev.y) * sp / d });
			}
			out.push_back(cur);
		} else if (sp <= 0) {
			const int64_t d = sc - sp;
			out.push_back({ prev.x + (cur.x - prev.x) * sp / (sp - sc),
				prev.y + (cur.y - prev.y) * sp / (sp - sc) });
		}
	}
	return out;
}

std::vector<Pt> dedupe(const std::vector<Pt>& in, int64_t eps)
{
	std::vector<Pt> out;
	for (const Pt& p : in) {
		if (!out.empty()) {
			const Pt& l = out.back();
			if (p.x > l.x - eps && p.x < l.x + eps
				&& p.y > l.y - eps && p.y < l.y + eps) {
				continue;
			}
		}
		out.push_back(p);
	}
	while (out.size() > 1) {
		const Pt& f = out.front();
		const Pt& l = out.back();
		if (f.x > l.x - eps && f.x < l.x + eps
			&& f.y > l.y - eps && f.y < l.y + eps) {
			out.pop_back();
		} else {
			break;
		}
	}
	return out;
}

/* 线段 AB 与 CD 距离的平方(int64,内部用 double 求最近点也行,工具不做仿真) */
double segDistSq(Pt a, Pt b, Pt c, Pt d)
{
	auto clamp01 = [](double t) { return t < 0 ? 0 : (t > 1 ? 1 : t); };
	const double ux = (double)(b.x - a.x), uy = (double)(b.y - a.y);
	const double vx = (double)(d.x - c.x), vy = (double)(d.y - c.y);
	const double wx = (double)(a.x - c.x), wy = (double)(a.y - c.y);
	const double uu = ux * ux + uy * uy;
	const double vv = vx * vx + vy * vy;
	const double uv = ux * vx + uy * vy;
	const double uw = ux * wx + uy * wy;
	const double vw = vx * wx + vy * wy;
	const double den = uu * vv - uv * uv;
	double sc, tc;
	if (den == 0) {
		sc = 0;
		tc = (vv > 0) ? clamp01(vw / vv) : 0;
	} else {
		sc = clamp01((uv * vw - vv * uw) / den);
		tc = (uv * sc + vw) / vv;
		if (tc < 0) {
			tc = 0;
			sc = clamp01(uw / uu);
		} else if (tc > 1) {
			tc = 1;
			sc = clamp01((uv + uw) / uu);
		}
	}
	const double dx = wx + ux * sc - vx * tc;
	const double dy = wy + uy * sc - vy * tc;
	return dx * dx + dy * dy;
}

} // namespace

int main(int argc, char** argv)
{
	const char* out = "maps/default.map";
	int grid = 8;
	uint64_t seed = 7;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			out = argv[++i];
		} else if (!strcmp(argv[i], "--grid") && i + 1 < argc) {
			grid = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			seed = strtoull(argv[++i], nullptr, 10);
		}
	}
	if (grid < 3 || grid > 24) {
		puts("grid range: 3..24");
		return 1;
	}

	const int n = grid * grid;
	const int64_t BOX = 880000;
	const int64_t CELL = (2 * BOX) / grid;

	/* 抖动网格撒点 */
	uint64_t rng[4];
	rngSeed(rng, seed);
	std::vector<Pt> sites(n);
	for (int gy = 0; gy < grid; gy++) {
		for (int gx = 0; gx < grid; gx++) {
			const int64_t jx = (int64_t)(rngNext(rng) % (uint64_t)(CELL * 7 / 10))
				- CELL * 35 / 100;
			const int64_t jy = (int64_t)(rngNext(rng) % (uint64_t)(CELL * 7 / 10))
				- CELL * 35 / 100;
			sites[gy * grid + gx] = { -BOX + gx * CELL + CELL / 2 + jx,
				-BOX + gy * CELL + CELL / 2 + jy };
		}
	}

	/* 胞腔 = 边界盒被所有邻居的中垂线切割 */
	std::vector<std::vector<Pt>> cells(n);
	const std::vector<Pt> box = { Pt{ -BOX, -BOX }, Pt{ BOX, -BOX },
		Pt{ BOX, BOX }, Pt{ -BOX, BOX } };
	for (int i = 0; i < n; i++) {
		std::vector<Pt> cell = box;
		for (int j = 0; j < n && cell.size() >= 3; j++) {
			if (j == i) {
				continue;
			}
			cell = clipByBisector(cell, sites[i], sites[j]);
		}
		cells[i] = dedupe(cell, 1500);
		if (cells[i].size() < 3) {
			printf("cell %d degenerate\n", i);
			return 2;
		}
	}

	/* 共享边 → 邻接(两侧胞腔边界线段距离 < eps) */
	std::vector<std::vector<ProvinceId>> adj(n);
	const double epsSq = 1200.0 * 1200.0;
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			bool near = false;
			for (size_t a = 0; a < cells[i].size() && !near; a++) {
				const Pt a1 = cells[i][a];
				const Pt a2 = cells[i][(a + 1) % cells[i].size()];
				for (size_t b = 0; b < cells[j].size() && !near; b++) {
					const Pt b1 = cells[j][b];
					const Pt b2 = cells[j][(b + 1) % cells[j].size()];
					if (segDistSq(a1, a2, b1, b2) < epsSq) {
						near = true;
					}
				}
			}
			if (near) {
				adj[i].push_back((ProvinceId)j);
				adj[j].push_back((ProvinceId)i);
			}
		}
	}

	/* 连通性检查(孤岛省会让寻路失败) */
	std::vector<int> comp(n, -1);
	int comps = 0;
	for (int i = 0; i < n; i++) {
		if (comp[i] >= 0) {
			continue;
		}
		std::vector<int> stack{ i };
		comp[i] = comps;
		while (!stack.empty()) {
			const int c = stack.back();
			stack.pop_back();
			for (ProvinceId nb : adj[c]) {
				if (comp[nb] < 0) {
					comp[nb] = comps;
					stack.push_back(nb);
				}
			}
		}
		comps++;
	}

	GameState st;
	st.map.count = (uint16_t)n;
	st.map.adj = adj;
	st.map.centerX.resize(n);
	st.map.centerY.resize(n);
	st.map.polyX.resize(n);
	st.map.polyY.resize(n);
	for (int i = 0; i < n; i++) {
		int64_t sx = 0, sy = 0;
		for (const Pt& p : cells[i]) {
			sx += p.x;
			sy += p.y;
			st.map.polyX[i].push_back((int32_t)p.x);
			st.map.polyY[i].push_back((int32_t)p.y);
		}
		st.map.centerX[i] = (int32_t)(sx / (int64_t)cells[i].size());
		st.map.centerY[i] = (int32_t)(sy / (int64_t)cells[i].size());
	}

	if (!saveMapFile(out, st)) {
		printf("cannot write: %s\n", out);
		return 3;
	}
	printf("map saved: %s (%d provinces, %d components", out, n, comps);
	if (comps != 1) {
		printf(" -- WARNING: disconnected!");
	}
	printf(")\n");
	return comps == 1 ? 0 : 4;
}
