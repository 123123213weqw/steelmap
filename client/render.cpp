/* M1 渲染:真实省界多边形(来自快照,客户端无需地图文件)+ ImGui 标签面板。
 * 踩坑结论(全部已固化,勿回退):
 *   - SDL 3.4:SDL_SetRenderDrawColorFloat 才收浮点色;RenderGeometry 必须带索引;
 *     几何体绑 1×1 白纹理;ReadPixels 返回 SDL_Surface*;RGBA8888 内存序 A,B,G,R
 *   - 按需重绘:静止 4fps,输入/动画满速;显式开垂直同步
 * 操作:左键点省=下移动令;右键拖=平移;滚轮=缩放;ESC 退出。
 */
#include "render.hpp"
#include <mapbox/earcut.hpp>
#include "../core/command.hpp"
#include "../net/protocol.hpp"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <math.h>
#include <utility>
#include <set>
#include <vector>

namespace gs {

namespace {

struct Vec2 {
	float x, y;
};

struct Color3 {
	float r, g, b;
};

const Color3 PLAYER_COLOR[5] = {
	{ 0.90f, 0.28f, 0.28f },
	{ 0.30f, 0.55f, 0.95f },
	{ 0.35f, 0.80f, 0.40f },
	{ 0.95f, 0.80f, 0.30f },
	{ 0.60f, 0.60f, 0.60f },
};

constexpr int CIRCLE_SEGS = 14;

struct MapCache {
	uint16_t count = 0;
	std::vector<Vec2> centers;
	std::vector<std::vector<Vec2>> polys;
	std::vector<std::vector<int>> tris;
	std::vector<std::pair<Vec2, Vec2>> borders;   /* 去重共享边 */
	std::vector<float> radii;                    /* 省平均半径(标签 LOD) */
	Vec2 boundMin{ 0, 0 }, boundMax{ 0, 0 };
};

MapCache buildCache(const GameState& st)
{
	MapCache c;
	c.count = st.map.count;
	c.centers.resize(st.map.count);
	c.polys.resize(st.map.count);
	c.tris.resize(st.map.count);
	c.radii.assign(st.map.count, 0.0f);
	bool first = true;
	for (uint16_t p = 0; p < st.map.count; p++) {
		c.centers[p] = { st.map.centerX[p] / 1000.0f,
			st.map.centerY[p] / 1000.0f };
		c.polys[p].reserve(st.map.polyX[p].size());
		for (size_t k = 0; k < st.map.polyX[p].size(); k++) {
			const Vec2 v{ st.map.polyX[p][k] / 1000.0f,
				st.map.polyY[p][k] / 1000.0f };
			if (!c.polys[p].empty()
				&& c.polys[p].back().x == v.x
				&& c.polys[p].back().y == v.y) {
				continue;                  /* 连续重复点(折返) */
			}
			c.polys[p].push_back(v);
			if (first) {
				c.boundMin = c.boundMax = v;
				first = false;
			} else {
				if (v.x < c.boundMin.x) c.boundMin.x = v.x;
				if (v.y < c.boundMin.y) c.boundMin.y = v.y;
				if (v.x > c.boundMax.x) c.boundMax.x = v.x;
				if (v.y > c.boundMax.y) c.boundMax.y = v.y;
			}
		}
		if (c.polys[p].size() > 1 && c.polys[p][0].x == c.polys[p].back().x
			&& c.polys[p][0].y == c.polys[p].back().y) {
			c.polys[p].pop_back();          /* 闭合重复 */
		}
		float rSum = 0.0f;
		for (const Vec2& v : c.polys[p]) {
			const float dx = v.x - c.centers[p].x;
			const float dy = v.y - c.centers[p].y;
			rSum += sqrtf(dx * dx + dy * dy);
		}
		c.radii[p] = rSum / (float)c.polys[p].size();
		{
			using P = std::pair<double, double>;
			std::vector<std::vector<P>> rings(1);
			rings[0].reserve(c.polys[p].size());
			for (const Vec2& v : c.polys[p]) {
				rings[0].emplace_back(v.x, v.y);
			}
			std::vector<uint32_t> tri =
				mapbox::earcut<uint32_t>(rings);
			c.tris[p].assign(tri.begin(), tri.end());
		}
	}
	/* 共享边去重:相邻省的同一条边界只保留一份。
	 * 拓扑简化保证两侧坐标完全一致,按毫单位量化做键。 */
	{
		struct EdgeKey {
			long long a, b, cc, dd;
			bool operator<(const EdgeKey& o) const {
				if (a != o.a) return a < o.a;
				if (b != o.b) return b < o.b;
				if (cc != o.cc) return cc < o.cc;
				return dd < o.dd;
			}
		};
		std::set<EdgeKey> seen;
		for (uint16_t p = 0; p < c.count; p++) {
			const std::vector<Vec2>& poly = c.polys[p];
			for (size_t k = 0; k < poly.size(); k++) {
				const Vec2 u = poly[k];
				const Vec2 v = poly[(k + 1) % poly.size()];
				EdgeKey ek{ (long long)llroundf(u.x * 1000.0f),
					(long long)llroundf(u.y * 1000.0f),
					(long long)llroundf(v.x * 1000.0f),
					(long long)llroundf(v.y * 1000.0f) };
				EdgeKey rev{ ek.cc, ek.dd, ek.a, ek.b };
				if (ek < rev) {
					if (seen.insert(ek).second) {
						c.borders.push_back({ u, v });
					}
				} else {
					if (seen.insert(rev).second) {
						c.borders.push_back({ u, v });
					}
				}
			}
		}
	}
	return c;
}

/* 三角化由 mapbox/earcut.hpp 承担(vendored,ISC)。
 * 手写耳切的血泪史(尖刺死锁/放射扇形/实现分叉)见 docs/PITFALLS.md G8。 */
bool pointInPoly(const std::vector<Vec2>& poly, Vec2 w)
{
	bool inside = false;
	const size_t n = poly.size();
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		if ((poly[i].y > w.y) != (poly[j].y > w.y)) {
			const float t = (w.y - poly[i].y) / (poly[j].y - poly[i].y);
			if (w.x < poly[i].x + t * (poly[j].x - poly[i].x)) {
				inside = !inside;
			}
		}
	}
	return inside;
}

int pickProvince(const MapCache& c, Vec2 world)
{
	for (uint16_t p = 0; p < c.count; p++) {
		if (pointInPoly(c.polys[p], world)) {
			return p;
		}
	}
	return -1;
}

struct Camera {
	float scale = 60;
	Vec2 offset{ 640, 400 };

	Vec2 toScreen(Vec2 w) const
	{
		return { w.x * scale + offset.x, w.y * scale + offset.y };
	}
	Vec2 toWorld(Vec2 s) const
	{
		return { (s.x - offset.x) / scale, (s.y - offset.y) / scale };
	}
};

struct UnitScreen {
	Vec2 s{ 0, 0 };
	bool valid = false;
};

struct UnitAnim {
	Vec2 from{}, to{};
	Uint64 t0 = 0;
	bool started = false;
};

bool writeBMP(const char* path, const uint8_t* mem, int w, int h, int pitch)
{
	FILE* f = fopen(path, "wb");
	if (f == nullptr) {
		return false;
	}
	uint8_t header[54] = { 0 };
	const uint32_t size = 54 + (uint32_t)(w * 4 * h);
	header[0] = 'B';
	header[1] = 'M';
	memcpy(header + 2, &size, 4);
	header[10] = 54;
	header[14] = 40;
	const int32_t iw = w, ih = h;
	memcpy(header + 18, &iw, 4);
	memcpy(header + 22, &ih, 4);
	header[26] = 1;
	header[28] = 32;
	fwrite(header, 1, 54, f);
	std::vector<uint8_t> row((size_t)w * 4);
	for (int y = h - 1; y >= 0; y--) {
		const uint8_t* src = mem + (size_t)y * pitch;
		for (int x = 0; x < w; x++) {
			row[x * 4 + 0] = src[x * 4 + 1];
			row[x * 4 + 1] = src[x * 4 + 2];
			row[x * 4 + 2] = src[x * 4 + 3];
			row[x * 4 + 3] = 0;
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
	return true;
}

} // namespace

int runRenderWindow(SharedView& view, int myPlayer,
	const std::function<void(uint16_t, uint16_t, uint16_t)>& sendCommand,
	const std::function<void(uint16_t seat)>& sendJoin,
	const char* shotPath, uint32_t shotAtTick)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		printf("[render] SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("steelmap M1",
		1280, 800,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (window == nullptr) {
		printf("[render] window failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Renderer* ren = SDL_CreateRenderer(window, nullptr);
	if (ren == nullptr) {
		printf("[render] renderer failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetRenderVSync(ren, 1);
	/* HiDPI:渲染按设备像素出图(2x),绘制坐标仍是逻辑点 */
	float dpi = 1.0f;
	{
		int wp = 0, hp = 0, op = 0, opp = 0;
		SDL_GetWindowSize(window, &wp, &hp);
		SDL_GetRenderOutputSize(ren, &op, &opp);
		if (wp > 0 && op > wp) {
			dpi = (float)op / (float)wp;
		}
		SDL_SetRenderScale(ren, dpi, dpi);
	}

	SDL_Texture* whiteTex = SDL_CreateTexture(ren,
		SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
	{
		const Uint32 white = 0xFFFFFFFFu;
		SDL_UpdateTexture(whiteTex, nullptr, &white, 4);
	}

	const bool useImGui = (getenv("NOIMGUI") == nullptr);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImFont* cjk = nullptr;
	if (useImGui) {
		ImGuiIO& io = ImGui::GetIO();
		ImFontConfig cfg;
		const float fontPx = 20.0f * dpi;
		io.FontGlobalScale = 1.0f / dpi;
		const char* fonts[] = {
			"/System/Library/Fonts/PingFang.ttc",
			"/System/Library/Fonts/Hiragino Sans GB.ttc",
			"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
			"/System/Library/Fonts/STHeiti Light.ttc",
		};
		for (const char* f : fonts) {
			cjk = io.Fonts->AddFontFromFileTTF(f, fontPx, &cfg,
				nullptr);   /* 1.92 动态字体:按需光栅化 CJK */
			if (cjk != nullptr) {
				break;
			}
		}
		if (cjk == nullptr) {
			io.Fonts->AddFontDefault();       /* 退回内置英文位图字体 */
		}
		ImGui_ImplSDL3_InitForSDLRenderer(window, ren);
		ImGui_ImplSDLRenderer3_Init(ren);
	}

	Camera cam;
	MapCache cache;
	std::vector<UnitScreen> unitScreenPos;
	std::vector<UnitAnim> anim(8);
	Vec2 mouseWorld{ 0, 0 };
	bool panning = false;
	Vec2 panGrab{};
	bool quit = false;
	bool shotTaken = false;
	int shotDelay = 0;
	bool dirty = true;
	Uint64 lastDraw = 0;
	bool wantBuild = false;
	int selectedUnit = -1;                 /* 单位选中:任意己方单位下令 */
	bool wantTab = false;
	Vec2 pendingClick{ 0, 0 };
	bool hasPendingClick = false;
	bool showHelp = false;
	bool showHelpDirty = false;
	bool leftPanning = false;
	bool leftMoved = false;
	Vec2 leftDown{ 0, 0 };
	uint32_t speedMs = 250;
	uint32_t lastSpeedMs = 250;
	bool wantSpeed = false;
	uint32_t lastSeenTick = 0xFFFFFFFFu;
	Uint64 lastSnapMs = SDL_GetTicks();
	uint64_t lastSeq = 0;

	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			dirty = true;
			if (useImGui) {
				ImGui_ImplSDL3_ProcessEvent(&ev);
			}
			if (ev.type == SDL_EVENT_QUIT) {
				quit = true;
			} else if (ev.type == SDL_EVENT_KEY_DOWN
				&& ev.key.key == SDLK_ESCAPE) {
				quit = true;
			} else if (ev.type == SDL_EVENT_KEY_DOWN
				&& ev.key.key == SDLK_B && myPlayer >= 0) {
				wantBuild = true;                /* 快照到位后处理 */
			} else if (ev.type == SDL_EVENT_KEY_DOWN
				&& ev.key.key == SDLK_H) {
				showHelp = !showHelp;
				dirty = true;
			} else if (ev.type == SDL_EVENT_KEY_DOWN
				&& (ev.key.key == SDLK_SPACE
					|| ev.key.key == SDLK_KP_PLUS
					|| ev.key.key == SDLK_KP_MINUS
					|| ev.key.key == SDLK_EQUALS
					|| ev.key.key == SDLK_MINUS)) {
				if (ev.key.key == SDLK_SPACE) {
					speedMs = (speedMs == 0) ? lastSpeedMs : 0;
				} else {
					if (speedMs == 0) {
						speedMs = lastSpeedMs;
					}
					if (ev.key.key == SDLK_KP_PLUS
						|| ev.key.key == SDLK_EQUALS) {
						speedMs = speedMs / 2;
						if (speedMs < 50) {
							speedMs = 50;
						}
					} else {
						speedMs = speedMs * 2;
						if (speedMs > 2000) {
							speedMs = 2000;
						}
					}
					lastSpeedMs = speedMs;
				}
				wantSpeed = true;
				dirty = true;
			} else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
				if (SDL_GetModState()
					& (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
					/* ctrl/cmd+滚动 = 缩放(以光标为中心) */
					const float f = (ev.wheel.y > 0) ? 1.15f
						: (1 / 1.15f);
					float mx, my;
					SDL_GetMouseState(&mx, &my);
					const Vec2 w = cam.toWorld({ mx, my });
					cam.scale *= f;
					cam.offset.x = mx - w.x * cam.scale;
					cam.offset.y = my - w.y * cam.scale;
				} else {
					/* 双指滚动 = 平移(macOS 惯例,
					 * 内容跟随手指) */
					cam.offset.x += ev.wheel.x * 14.0f;
					cam.offset.y -= ev.wheel.y * 14.0f;
				}
			} else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN
				&& ev.button.button == SDL_BUTTON_RIGHT) {
				panning = true;
				panGrab = { ev.button.x, ev.button.y };
			} else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP
				&& ev.button.button == SDL_BUTTON_RIGHT) {
				panning = false;
			} else if (ev.type == SDL_EVENT_MOUSE_MOTION && panning) {
				cam.offset.x += ev.motion.x - panGrab.x;
				cam.offset.y += ev.motion.y - panGrab.y;
				panGrab = { ev.motion.x, ev.motion.y };
			} else if (ev.type == SDL_EVENT_KEY_DOWN
				&& ev.key.key == SDLK_TAB) {
				wantTab = true;                     /* 快照到位后循环选中 */
			} else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN
				&& ev.button.button == SDL_BUTTON_LEFT
				&& myPlayer >= 0 && cache.count > 0) {
				pendingClick = { ev.button.x, ev.button.y };
					hasPendingClick = true;
			}
		}

		GameState st;
		uint64_t hashNow = 0;
		bool hasNow = false;
		std::vector<EventLine> events;
		{
			std::lock_guard<std::mutex> lk(view.mu);
			st = view.state;
			hashNow = view.hash;
			hasNow = view.has;
			events = view.events;
			if (view.snapSeq != lastSeq) {
				lastSeq = view.snapSeq;
				lastSnapMs = SDL_GetTicks();   /* 任何快照都算活(含大厅) */
			}
		}
		if (!hasNow) {
			st = GameState{};
		}

		if (cache.count != st.map.count) {
			cache = buildCache(st);
			int w = 1280, h = 800;
			SDL_GetWindowSize(window, &w, &h);
			const float spanX = cache.boundMax.x - cache.boundMin.x;
			const float spanY = cache.boundMax.y - cache.boundMin.y;
			const float span = (spanX > spanY ? spanX : spanY) * 1.1f;
			if (span > 1.0f) {
				const float fit = (float)w * 0.9f / span;
				cam.scale = fit * 3.5f;      /* 开局:出生地特写 */
				Vec2 focus{ (cache.boundMin.x + cache.boundMax.x) / 2,
					(cache.boundMin.y + cache.boundMax.y) / 2 };
				for (const Unit& u : st.units) {
					if (u.strength > 0 && u.fog == 0
						&& (int)u.owner == myPlayer
						&& u.province < cache.count) {
						focus = cache.centers[u.province];
						break;
					}
				}
				cam.offset = { (float)w / 2 - focus.x * cam.scale,
					(float)h / 2 - focus.y * cam.scale };
			}
			for (auto& a : anim) {
				a.started = false;
			}
		}
		if (wantSpeed) {
			wantSpeed = false;
			sendCommand((uint16_t)CmdType::SetSpeed, 0,
				(uint16_t)speedMs);
		}
		if ((wantTab || hasPendingClick || showHelpDirty) && hasNow) {
			showHelpDirty = false;
			if (wantTab) {
				wantTab = false;
				int next = -1;
				const int n = (int)st.units.size();
				for (int k = 1; k <= n; k++) {
					const int i = (selectedUnit + k) % n;
					if (st.units[i].strength > 0 && st.units[i].fog == 0
						&& st.units[i].owner == myPlayer) {
						next = i;
						break;
					}
				}
				selectedUnit = next;
			}
			if (hasPendingClick) {
				hasPendingClick = false;
				/* 先测点中己方单位 */
				int hitUnit = -1;
				float bestD = 26.0f;
				for (size_t i = 0; i < st.units.size(); i++) {
					if (st.units[i].strength <= 0 || st.units[i].fog != 0
						|| st.units[i].owner != myPlayer
						|| st.units[i].province >= cache.count) {
						continue;
					}
					const Vec2 s = cam.toScreen(
						cache.centers[st.units[i].province]);
					const float dx = s.x - pendingClick.x;
					const float dy = s.y - pendingClick.y;
					const float d = sqrtf(dx * dx + dy * dy);
					if (d < bestD) {
						bestD = d;
						hitUnit = (int)i;
					}
				}
				if (hitUnit >= 0) {
					selectedUnit = (selectedUnit == hitUnit)
						? -1 : hitUnit;
				} else {
					const int hit = pickProvince(cache,
						cam.toWorld(pendingClick));
					if (hit >= 0 && selectedUnit >= 0) {
						sendCommand((uint16_t)CmdType::MoveUnit,
							(uint16_t)selectedUnit, (uint16_t)hit);
						/* 乐观反馈:立刻画目标线,下一拍快照校正 */
						std::lock_guard<std::mutex> lk(view.mu);
						if ((size_t)selectedUnit
							< view.state.units.size()) {
							view.state.units[selectedUnit].target =
								(ProvinceId)hit;
						}
						dirty = true;
					}
				}
			}
		}
		if (wantBuild && hasNow) {
			wantBuild = false;
			for (uint16_t p = 0; p < st.map.count; p++) {
				if (p < st.provinceOwner.size()
					&& st.provinceOwner[p] == (PlayerId)myPlayer) {
					sendCommand((uint16_t)CmdType::BuildUnit, 0, p);
					break;
				}
			}
		}
		if (st.tick != lastSeenTick) {
			if (lastSeenTick != 0xFFFFFFFFu) {
				for (size_t i = 0; i < st.units.size() && i < anim.size();
					i++) {
					const Vec2 dst = cache.centers[st.units[i].province];
					if (!anim[i].started) {
						anim[i].from = dst;
						anim[i].started = true;
					} else {
						anim[i].from = anim[i].to;
					}
					anim[i].to = dst;
					anim[i].t0 = SDL_GetTicks();
				}
			}
			lastSeenTick = st.tick;
			dirty = true;
		}
		float mx, my;
		SDL_GetMouseState(&mx, &my);
		mouseWorld = cam.toWorld({ mx, my });

		{
			const Uint64 nowMs = SDL_GetTicks();
			if (hasNow && nowMs - lastSnapMs > 15000) {
				printf("[render] no snapshot for 15s, exiting\n");
				break;
			}
			bool animating = false;
			for (const UnitAnim& a : anim) {
				if (a.started && nowMs - a.t0 < 260) {
					animating = true;
					break;
				}
			}
			if (!dirty && !animating && nowMs - lastDraw < 250) {
				SDL_Delay(4);
				continue;
			}
			dirty = false;
			lastDraw = nowMs;
		}

		SDL_SetRenderDrawColorFloat(ren, 0.09f, 0.10f, 0.13f, 1.0f);
		SDL_RenderClear(ren);

		const int hover = cache.count > 0
			? pickProvince(cache, mouseWorld) : -1;

		if (cache.count > 0) {
			for (uint16_t p = 0; p < cache.count; p++) {
				const bool hot = ((int)p == hover);
				const PlayerId owner =
					(p < st.provinceOwner.size())
						? st.provinceOwner[p] : NEUTRAL_PLAYER;
				SDL_FColor fill{ 0.24f, 0.27f, 0.33f, 1.0f };
				const uint8_t terr = (p < st.map.terrain.size())
					? st.map.terrain[p] : (uint8_t)TERRAIN_PLAINS;
				if (terr == TERRAIN_MOUNTAIN) {       /* 山地:灰白 */
					fill = { 0.33f, 0.33f, 0.36f, 1.0f };
				} else if (terr == TERRAIN_FOREST) {  /* 森林:墨绿 */
					fill = { 0.20f, 0.30f, 0.24f, 1.0f };
				}
				if (owner >= 5 && cam.scale < 5.0f) {
					/* 远景中立省调暗:大陆剪影,迷雾层级 */
					fill.r *= 0.72f;
					fill.g *= 0.72f;
					fill.b *= 0.75f;
				}
				if (owner < 5) {           /* 归属色:底色 + 主色 45% */
					fill = SDL_FColor{
						fill.r * 0.45f + PLAYER_COLOR[owner].r * 0.55f,
						fill.g * 0.45f + PLAYER_COLOR[owner].g * 0.55f,
						fill.b * 0.45f + PLAYER_COLOR[owner].b * 0.55f,
						1.0f };
				}
				if (hot) {
					fill.r += 0.12f;
					fill.g += 0.12f;
					fill.b += 0.12f;
				}
				const std::vector<Vec2>& poly = cache.polys[p];
				std::vector<SDL_Vertex> verts;
				verts.reserve(poly.size());
				for (const Vec2& v : poly) {
					const Vec2 s = cam.toScreen(v);
					verts.push_back({ { s.x, s.y }, fill, { 0, 0 } });
				}
				if (!cache.tris[p].empty()) {
					(void)SDL_RenderGeometry(ren, whiteTex, verts.data(),
						(int)verts.size(), cache.tris[p].data(),
						(int)cache.tris[p].size());
				}
			}
			/* 基础边界:去重共享边,细线,屏上过短跳过。
			 * 远景(全景)时减淡 → 只剩大陆剪影,避免噪点。 */
			const float lod = cam.scale < 5.0f ? 0.28f : 0.9f;
			const SDL_FColor borderCol{ 0.52f, 0.56f, 0.64f, lod };
			std::vector<SDL_Vertex> edgeVerts;
			std::vector<int> edgeIdx;
			for (const auto& uv : cache.borders) {
				const Vec2 a = cam.toScreen(uv.first);
				const Vec2 b = cam.toScreen(uv.second);
				float dx = b.x - a.x, dy = b.y - a.y;
				const float len = sqrtf(dx * dx + dy * dy);
				if (len < 2.5f) {
					continue;              /* 远景细化:碎边不画 */
				}
				dx /= len;
				dy /= len;
				const float ox = -dy * 0.7f, oy = dx * 0.7f;
				const int base = (int)edgeVerts.size();
				const SDL_Vertex quad[4] = {
					{ { a.x + ox, a.y + oy }, borderCol, { 0, 0 } },
					{ { a.x - ox, a.y - oy }, borderCol, { 0, 0 } },
					{ { b.x - ox, b.y - oy }, borderCol, { 0, 0 } },
					{ { b.x + ox, b.y + oy }, borderCol, { 0, 0 } },
				};
				for (int q = 0; q < 4; q++) {
					edgeVerts.push_back(quad[q]);
				}
				const int six[6] = { base, base + 1, base + 2,
					base, base + 2, base + 3 };
				edgeIdx.insert(edgeIdx.end(), six, six + 6);
			}
			if (!edgeIdx.empty()) {
				(void)SDL_RenderGeometry(ren, whiteTex, edgeVerts.data(),
					(int)edgeVerts.size(), edgeIdx.data(),
					(int)edgeIdx.size());
			}
			/* 交战省:红色脉冲粗框(仅交战省的环,数量少) */
			{
				const float pulse = 0.5f + 0.5f
					* sinf((float)(SDL_GetTicks() % 600) / 600.0f
						* 6.2831853f);
				const SDL_FColor warCol{ 0.95f, 0.25f, 0.2f,
					0.55f + 0.45f * pulse };
				std::vector<SDL_Vertex> wv;
				std::vector<int> wi;
				for (uint16_t p = 0; p < cache.count; p++) {
					bool war = false;
					{
						PlayerId first = SERVER_PLAYER;
						for (const Unit& u : st.units) {
							if (u.province == p && u.strength > 0
								&& u.fog == 0) {
								if (first == SERVER_PLAYER) {
									first = u.owner;
								} else if (u.owner != first) {
									war = true;
									break;
								}
							}
						}
					}
					if (!war) {
						continue;
					}
					const std::vector<Vec2>& poly = cache.polys[p];
					for (size_t k = 0; k < poly.size(); k++) {
						const Vec2 a = cam.toScreen(poly[k]);
						const Vec2 b =
							cam.toScreen(poly[(k + 1) % poly.size()]);
						float dx = b.x - a.x, dy = b.y - a.y;
						const float len = sqrtf(dx * dx + dy * dy);
						if (len < 2.0f) {
							continue;
						}
						dx /= len;
						dy /= len;
						const float ox = -dy * 2.5f, oy = dx * 2.5f;
						const int base = (int)wv.size();
						const SDL_Vertex quad[4] = {
							{ { a.x + ox, a.y + oy }, warCol, { 0, 0 } },
							{ { a.x - ox, a.y - oy }, warCol, { 0, 0 } },
							{ { b.x - ox, b.y - oy }, warCol, { 0, 0 } },
							{ { b.x + ox, b.y + oy }, warCol, { 0, 0 } },
						};
						for (int q = 0; q < 4; q++) {
							wv.push_back(quad[q]);
						}
						const int six[6] = { base, base + 1, base + 2,
							base, base + 2, base + 3 };
						wi.insert(wi.end(), six, six + 6);
					}
				}
				if (!wi.empty()) {
					(void)SDL_RenderGeometry(ren, whiteTex, wv.data(),
						(int)wv.size(), wi.data(), (int)wi.size());
				}
			}
			if (!edgeIdx.empty()) {
				(void)SDL_RenderGeometry(ren, whiteTex, edgeVerts.data(),
					(int)edgeVerts.size(), edgeIdx.data(),
					(int)edgeIdx.size());
			}

			const Uint64 now = SDL_GetTicks();
			unitScreenPos.clear();
			unitScreenPos.resize(st.units.size());
			for (size_t i = 0; i < st.units.size() && i < anim.size();
				i++) {
				const Unit& u = st.units[i];
				if (u.strength <= 0 || u.fog != 0 || !anim[i].started) {
					continue;
				}
				const float unitR =
					0.020f + 0.016f * (float)u.strength / 100.0f;
				const float t = (now - anim[i].t0) >= 200
					? 1.0f
					: (float)(now - anim[i].t0) / 200.0f;
				const Vec2 pos{
					anim[i].from.x + (anim[i].to.x - anim[i].from.x) * t,
					anim[i].from.y + (anim[i].to.y - anim[i].from.y) * t };
				if (u.target != INVALID_PROVINCE) {
					const Vec2 dst =
						cam.toScreen(cache.centers[u.target]);
					const Vec2 src = cam.toScreen(pos);
					const SDL_FColor lineCol{ 1.0f, 1.0f, 1.0f, 0.35f };
					float dx = dst.x - src.x, dy = dst.y - src.y;
					const float len = sqrtf(dx * dx + dy * dy);
					if (len >= 2.0f) {
						dx /= len;
						dy /= len;
						const float ox = -dy, oy = dx;
						const SDL_Vertex seg[4] = {
							{ { src.x + ox, src.y + oy }, lineCol, { 0, 0 } },
							{ { src.x - ox, src.y - oy }, lineCol, { 0, 0 } },
							{ { dst.x - ox, dst.y - oy }, lineCol, { 0, 0 } },
							{ { dst.x + ox, dst.y + oy }, lineCol, { 0, 0 } },
						};
						const int six[6] = { 0, 1, 2, 0, 2, 3 };
						(void)SDL_RenderGeometry(ren, whiteTex, seg, 4,
							six, 6);
					}
				}
				const SDL_FColor col{ PLAYER_COLOR[u.owner % 5].r,
					PLAYER_COLOR[u.owner % 5].g,
					PLAYER_COLOR[u.owner % 5].b, 1.0f };
				std::vector<SDL_Vertex> verts;
				std::vector<int> idx;
				for (int k = 0; k <= CIRCLE_SEGS; k++) {
					const float a = (float)k
						* (float)(6.2831853f / CIRCLE_SEGS);
					const Vec2 pt = cam.toScreen({
						pos.x + cosf(a) * unitR,
						pos.y + sinf(a) * unitR });
					if (k == 0) {
						const Vec2 c = cam.toScreen(pos);
						verts.push_back({ { c.x, c.y }, col, { 0, 0 } });
					}
					verts.push_back({ { pt.x, pt.y }, col, { 0, 0 } });
					if (k >= 1) {
						const int tri[3] = { 0, k, k + 1 };
						idx.insert(idx.end(), tri, tri + 3);
					}
				}
				(void)SDL_RenderGeometry(ren, whiteTex, verts.data(),
					(int)verts.size(), idx.data(), (int)idx.size());
				unitScreenPos[i].s = cam.toScreen(pos);
				unitScreenPos[i].valid = true;
				if ((int)i == selectedUnit) {        /* 选中白环 */
					const SDL_FColor ring{ 1, 1, 1, 0.8f };
					std::vector<SDL_Vertex> rv;
					std::vector<int> ri;
					const float rr = unitR * 1.8f;
					for (int k = 0; k <= CIRCLE_SEGS; k++) {
						const float a = (float)k
							* (float)(6.2831853f / CIRCLE_SEGS);
						const Vec2 pt = cam.toScreen({ pos.x + cosf(a) * rr,
							pos.y + sinf(a) * rr });
						if (k == 0) {
							rv.push_back({ { pt.x, pt.y }, ring, { 0, 0 } });
						}
						rv.push_back({ { pt.x, pt.y }, ring, { 0, 0 } });
						if (k >= 1) {
							const int tri[3] = { 0, k, k + 1 };
							ri.insert(ri.end(), tri, tri + 3);
						}
					}
					for (auto& v : rv) {              /* 环=大圆挖小圆难,退化为亮圈 */
						v.color = SDL_FColor{ 1, 1, 1, 0.35f };
					}
					(void)SDL_RenderGeometry(ren, whiteTex, rv.data(),
						(int)rv.size(), ri.data(), (int)ri.size());
				}
			}
		}

		if (useImGui) {
			ImGui_ImplSDL3_NewFrame();
			ImGui_ImplSDLRenderer3_NewFrame();
			ImGui::NewFrame();
			{
				int wp = 1280, hp = 800;
				SDL_GetWindowSize(window, &wp, &hp);
				ImGui::GetIO().DisplaySize = { (float)wp,
					(float)hp };
			}
			{
				const char* PLAYER_NAME[5] = { "红方", "蓝方", "绿方",
					"黄方", "中立" };
				const ImVec4 PLAYER_IM[5] = {
					ImVec4(0.95f, 0.35f, 0.35f, 1),
					ImVec4(0.45f, 0.65f, 1.0f, 1),
					ImVec4(0.45f, 0.9f, 0.5f, 1),
					ImVec4(1.0f, 0.85f, 0.4f, 1),
					ImVec4(0.7f, 0.7f, 0.7f, 1),
				};
				/* 兵力数字 + 目的地标记 */
				ImDrawList* dl = ImGui::GetForegroundDrawList();
				int w0 = 1280, h0 = 800;
				SDL_GetWindowSize(window, &w0, &h0);
				for (uint16_t p = 0; p < cache.count; p++) {
					const Vec2 s = cam.toScreen(cache.centers[p]);
					if (s.x > -20 && s.y > -20 && s.x < w0 + 20
						&& s.y < h0 + 20) {
						char label[8];
						snprintf(label, sizeof(label), "%u",
							(unsigned)p);
						dl->AddText({ s.x - 5, s.y + 10 },
							IM_COL32(160, 165, 175, 200), label);
					}
				}
				for (size_t i = 0; i < st.units.size(); i++) {
					if (!unitScreenPos[i].valid) {
						continue;
					}
					char label[16];
					snprintf(label, sizeof(label), "%d",
						(int)st.units[i].strength);
					dl->AddText(
						{ unitScreenPos[i].s.x + 12,
							unitScreenPos[i].s.y - 12 },
						IM_COL32(255, 255, 255, 220), label);
				}

				/* 顶栏 */
				{
					int provCount[5] = { 0, 0, 0, 0, 0 };
					int aliveUnits[5] = { 0, 0, 0, 0, 0 };
					for (uint16_t p = 0; p < st.map.count; p++) {
						if (p < st.provinceOwner.size()
							&& st.provinceOwner[p] < 5) {
							provCount[st.provinceOwner[p]]++;
						}
					}
					for (const Unit& u : st.units) {
						if (u.strength > 0 && u.owner < 5) {
							aliveUnits[u.owner]++;
						}
					}
					ImGui::SetNextWindowPos(
						{ (float)w0 / 2 - 260, 8 },
						ImGuiCond_Always);
					ImGui::SetNextWindowSize({ 520, 64 },
						ImGuiCond_Always);
					ImGui::Begin("##topbar", nullptr,
						ImGuiWindowFlags_NoTitleBar
							| ImGuiWindowFlags_NoResize
							| ImGuiWindowFlags_NoScrollbar
							| ImGuiWindowFlags_NoCollapse
							| ImGuiWindowFlags_NoBackground);
					if (myPlayer >= 0 && myPlayer < 5) {
						ImGui::TextColored(PLAYER_IM[myPlayer], "%s",
							PLAYER_NAME[myPlayer]);
						ImGui::SameLine();
					}
					ImGui::Text(" %s第%u天  补给%u(+%u/天)  %s%ums/天",
						st.phase == 0 ? "[大厅] " : "",
						(unsigned)st.tick,
						(0 <= myPlayer && myPlayer < 16)
							? (unsigned)st.supplies[myPlayer] : 0u,
						(unsigned)(provCount[myPlayer < 0 ? 4
							: myPlayer] * 2),
						speedMs == 0 ? "[暂停] " : "",
						speedMs == 0 ? 0u : speedMs);
					ImGui::Text(" 部队%d  领土%d%s",
						(0 <= myPlayer && myPlayer < 5)
							? aliveUnits[myPlayer] : 0,
						(0 <= myPlayer && myPlayer < 5)
							? provCount[myPlayer] : 0,
						selectedUnit >= 0 ? "  [已选单位,点目标省下令]"
							: "");
					ImGui::End();
				}

				/* 战报(左下) */
				{
					ImGui::SetNextWindowPos({ 8, (float)h0 - 220 },
						ImGuiCond_Always);
					ImGui::SetNextWindowSize({ 420, 212 },
						ImGuiCond_Always);
					ImGui::Begin("战报", nullptr,
						ImGuiWindowFlags_NoCollapse);
					for (auto it = events.rbegin();
						it != events.rend(); ++it) {
						const char* nm = (it->player < 5)
							? PLAYER_NAME[it->player] : "?";
						const ImVec4& c = PLAYER_IM[it->player < 5
							? it->player : 4];
						if (it->type == (uint16_t)EventType::Capture) {
							ImGui::TextColored(c, "第%u天  %s 攻占 %u 省",
								(unsigned)it->day, nm,
								(unsigned)it->a);
						} else if (it->type
							== (uint16_t)EventType::Died) {
							ImGui::TextColored(c, "第%u天  %s 的部队 %u 全灭",
								(unsigned)it->day, nm,
								(unsigned)it->a);
						} else if (it->type
							== (uint16_t)EventType::Built) {
							ImGui::TextColored(c, "第%u天  %s 新建部队 %u",
								(unsigned)it->day, nm,
								(unsigned)it->a);
						} else if (it->type
							== (uint16_t)EventType::Winner) {
							ImGui::TextColored(c, "第%u天  %s 获胜!",
								(unsigned)it->day, nm);
						}
					}
					ImGui::End();
				}

				/* 大厅 */
				if (st.phase == 0) {
					ImGui::SetNextWindowPos(
						{ (float)w0 / 2 - 200, (float)h0 / 2 - 130 },
						ImGuiCond_Always);
					ImGui::SetNextWindowSize({ 400, 260 },
						ImGuiCond_Always);
					ImGui::Begin("大厅", nullptr,
						ImGuiWindowFlags_NoCollapse);
					ImGui::TextUnformatted("选择座位(点色块换座):");
					for (int s = 0; s < 4; s++) {
						ImGui::PushStyleColor(ImGuiCol_Button,
							PLAYER_IM[s]);
						ImGui::PushStyleColor(ImGuiCol_Text,
							ImVec4(0, 0, 0, 1));
						char name[32];
						snprintf(name, sizeof(name), "%s%s##%d",
							PLAYER_NAME[s],
							(myPlayer == s) ? " (我)" : "", s);
						if (ImGui::Button(name, { 180, 40 })
							&& myPlayer != s) {
							sendJoin((uint16_t)s);
						}
						ImGui::PopStyleColor(2);
						if (s % 2 == 0) {
							ImGui::SameLine();
						}
					}
					ImGui::Separator();
					if (myPlayer == 0) {
						if (ImGui::Button("开始游戏(房主)",
							{ 180, 46 })) {
							sendCommand((uint16_t)CmdType::StartGame,
								0, 0);
						}
					} else {
						ImGui::TextUnformatted("等待房主开始...");
					}
					ImGui::End();
				}

				/* 帮助 */
				if (showHelp) {
					ImGui::SetNextWindowPos({ (float)w0 / 2 - 240,
						(float)h0 / 2 - 160 }, ImGuiCond_FirstUseEver);
					ImGui::Begin("帮助 (H 开关)", &showHelp);
					ImGui::TextUnformatted(
						"目标:消灭其他所有势力\n"
						"\n"
						"[操作]\n"
						" 左键点自己部队 = 选中/取消\n"
						" Tab = 循环选中自己的部队\n"
						" 选中后再点省份 = 行军令\n"
						" B = 花补建造新部队(在最早的己方省)\n"
						" 拖动或双指滚动 = 平移, ctrl/⌘+滚动 = 缩放\n"
						" 空格 = 暂停, +/- = 减速/加速\n"
						"\n"
						"[规则]\n"
						" 每占领一省:补给日入 +2\n"
						" 部队驻己方省:每天回血 +2\n"
						" 两军同省即交战,每日互掷 8-15 伤害\n"
						" 兵力归零 = 全灭;红框闪烁的省正在交战");
					ImGui::End();
				}

				/* 悬停提示 */
				{
					int hoverUnit = -1;
					float bestD = 26.0f;
					for (size_t i = 0; i < st.units.size(); i++) {
						if (!unitScreenPos[i].valid
							|| st.units[i].strength <= 0
							|| st.units[i].fog != 0) {
							continue;
						}
						const float dx = unitScreenPos[i].s.x - mx;
						const float dy = unitScreenPos[i].s.y - my;
						const float d = sqrtf(dx * dx + dy * dy);
						if (d < bestD) {
							bestD = d;
							hoverUnit = (int)i;
						}
					}
					if (hoverUnit >= 0) {
						const Unit& u = st.units[hoverUnit];
						ImGui::BeginTooltip();
						ImGui::Text("部队 %u  %s",
							(unsigned)hoverUnit,
							(u.owner < 5) ? PLAYER_NAME[u.owner] : "?");
						ImGui::Text("兵力 %d%s", (int)u.strength,
							u.target == INVALID_PROVINCE ? ""
								: "  行军中");
						ImGui::EndTooltip();
					} else if (hover >= 0 && hover < (int)cache.count) {
						const PlayerId o =
							((size_t)hover < st.provinceOwner.size())
								? st.provinceOwner[hover]
								: NEUTRAL_PLAYER;
						ImGui::BeginTooltip();
						ImGui::Text("省份 %u  %s", (unsigned)hover,
							(o < 5) ? PLAYER_NAME[o] : "中立");
						ImGui::Text("收入 +2/天");
						ImGui::EndTooltip();
					}
				}
			}
			ImGui::Render();
			ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),
				ren);
		}

		/* 截图:Present 之前回读当前 drawable(同帧内容,
		 * ReadPixels 会强制 flush;Present 后读=翻转后的空帧) */
		if (shotPath != nullptr && !shotTaken) {
			if (hasNow && st.tick >= shotAtTick && shotDelay == 0) {
				shotDelay = 2;                /* 再画 2 帧后读 */
			} else if (shotDelay > 0 && --shotDelay == 0) {
				SDL_Surface* surf = SDL_RenderReadPixels(ren, nullptr);
				if (surf != nullptr) {
					SDL_Surface* conv = SDL_ConvertSurface(surf,
						SDL_PIXELFORMAT_RGBA8888);
					SDL_DestroySurface(surf);
					if (conv != nullptr) {
						if (writeBMP(shotPath,
							(const uint8_t*)conv->pixels, conv->w,
							conv->h, (int)conv->pitch)) {
							printf("[render] screenshot saved: %s"
								" (tick %u)\n", shotPath,
								(unsigned)st.tick);
						}
						SDL_DestroySurface(conv);
					}
				}
				shotTaken = true;
				quit = true;
			}
		}

		SDL_RenderPresent(ren);
	}

	if (useImGui) {
		ImGui_ImplSDLRenderer3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
	}
	ImGui::DestroyContext();
	SDL_DestroyTexture(whiteTex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}

} // namespace gs
