#!/usr/bin/env python3
"""真实世界地图管线 v2:省/州级(admin-1)+ 拓扑简化。

v1 的教训:各国边界独立简化 → 共享边界不重合 → 接缝空缺。
v2:先建共享边(Natural Earth 相邻多边形在共同边界上坐标逐点相同),
每条共享边只简化一次,各国从同一份简化边重建 → 缝隙从数学上消失。

用法: python3 tools/worldmap.py [输入.geojson] [输出.map]
数据: Natural Earth 50m admin-1(省/州,public domain)
"""
import json
import math

from shapely.geometry import Polygon
try:
    from shapely import make_valid
except ImportError:
    from shapely.validation import make_valid
import struct
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "/tmp/m0/admin1.geojson"
DST = sys.argv[2] if len(sys.argv) > 2 else "maps/world.map"

EDGE_EPS = 0.10          # 度(共享边简化)
MERGE_EPS = 0.06         # 度(重建后共线合并,压顶点数)
MIN_AREA_SQ_DEG = 0.08   # 丢小岛
MOUNTAIN_LAT = 48
FOREST_LOW_LAT = 8


def key_of(pt):
    return (round(pt[0], 6), round(pt[1], 6))


def dp(pts, eps):
    """端点固定的 Douglas-Peucker,简化内部点。"""
    if len(pts) < 3:
        return pts
    keep = [True] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        lo, hi = stack.pop()
        if hi <= lo + 1:
            continue
        ax, ay = pts[lo]
        bx, by = pts[hi]
        dx, dy = bx - ax, by - ay
        seg = math.hypot(dx, dy)
        best, bi = -1.0, -1
        for i in range(lo + 1, hi):
            px, py = pts[i]
            if seg < 1e-12:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dy * (px - ax) - dx * (py - ay)) / seg
            if d > best:
                best, bi = d, i
        if best > eps:
            stack.append((lo, bi))
            stack.append((bi, hi))
        else:
            for i in range(lo + 1, hi):
                keep[i] = False
    return [p for p, k in zip(pts, keep) if k]


def seg_cross(a, b, c, d):
    """线段 AB 与 CD 是否规范相交(不含共享端点)。"""
    def cr(o, p, q):
        return (p[0]-o[0])*(q[1]-o[1]) - (p[1]-o[1])*(q[0]-o[0])
    d1 = cr(a, b, c)
    d2 = cr(a, b, d)
    d3 = cr(c, d, a)
    d4 = cr(c, d, b)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0))


def is_simple(pts):
    """环是否自交(相邻边共享端点不算)。"""
    n = len(pts)
    for i in range(n):
        a1, a2 = pts[i], pts[(i + 1) % n]
        for j in range(i + 2, n):
            b1, b2 = pts[j], pts[(j + 1) % n]
            if i == 0 and j == n - 1:
                continue
            if seg_cross(a1, a2, b1, b2):
                return False
    return True


def merge_collinear(pts, eps):
    """局部确定性共线合并:中间点距前后连线 < eps 即删。
    两省共享边上的点局部几何完全相同 → 合并决策相同 → 拓扑不破。"""
    def dist_seg(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        seg2 = dx * dx + dy * dy
        if seg2 < 1e-18:
            return math.hypot(px - ax, py - ay)
        t = ((px - ax) * dx + (py - ay) * dy) / seg2
        t = 0.0 if t < 0 else (1.0 if t > 1 else t)
        return math.hypot(px - ax - t * dx, py - ay - t * dy)

    cur = pts
    while True:
        n = len(cur)
        if n < 8:
            return cur
        out = []
        for i in range(n):
            a = cur[(i - 1) % n]
            b = cur[i]
            c = cur[(i + 1) % n]
            if dist_seg(b[0], b[1], a[0], a[1], c[0], c[1]) < eps:
                continue
            out.append(b)
        if len(out) >= 8 and len(out) < n:
            cur = out
        else:
            return cur


def main():
    data = json.load(open(SRC))

    # ---- 1. 收集要素与环(外环),建共享边表 ----
    feats = []            # [{name, nation, rings:[[pt...] 有序]}]
    edges = {}            # ekey -> {'pts':[...], 'users':[(fi,ri,fwd)]}
    for fi, f in enumerate(data["features"]):
        g = f["geometry"]
        if g is None:
            continue
        nation = f["properties"].get("admin", "?")
        name = f["properties"].get("name", "?")
        if nation == "Antarctica":
            continue
        polys = (g["coordinates"] if g["type"] == "MultiPolygon"
                 else [g["coordinates"]])
        rings = []
        for poly in polys:
            ring = [key_of(p) for p in poly[0]]
            # 去连续重复点
            dedup = [ring[0]]
            for p in ring[1:]:
                if p != dedup[-1]:
                    dedup.append(p)
            if len(dedup) > 2 and dedup[0] == dedup[-1]:
                dedup.pop()
            if len(dedup) < 4:
                continue
            rings.append(dedup)
        if not rings:
            continue
        feats.append({"name": name, "nation": nation, "rings": rings})
        fi2 = len(feats) - 1
        for ri, ring in enumerate(rings):
            n = len(ring)
            seq = []
            for i in range(n):
                p = ring[i]
                q = ring[(i + 1) % n]
                ek = (p, q) if p <= q else (q, p)
                e = edges.get(ek)
                if e is None:
                    e = edges[ek] = {"pts": [list(p), list(q)], "users": []}
                seq.append((ek, p == ek[0]))
            feats[fi2]["rings"][ri] = seq      # 临时换成边序列

    print(f"features={len(feats)} shared_edges={len(edges)}")

    # ---- 2. 每条共享边只简化一次 ----
    for e in edges.values():
        e["simp"] = [tuple(p) for p in dp(e["pts"], EDGE_EPS)]

    # ---- 3. 由共享边重建环(相邻单位逐点一致 → 无缝隙) ----
    provinces = []
    for fi2, f in enumerate(feats):
        for ri, seq in enumerate(f["rings"]):
            pts = []
            for ek, fwd in seq:
                seg = edges[ek]["simp"]
                piece = seg if fwd else list(reversed(seg))
                pts.extend(piece[:-1])
            merged = merge_collinear(pts, MERGE_EPS)
            if len(merged) >= 4 and is_simple(merged):
                pts = merged
            else:
                merged2 = merge_collinear(pts, MERGE_EPS / 2)
                if len(merged2) >= 4 and is_simple(merged2):
                    pts = merged2
            if not is_simple(pts):
                # 原始数据即自交(Natural Earth 脏数据):shapely 修复,
                # 取最大有效外环。不修则耳切部分失败 → 放射扇形残缺。
                geom = make_valid(Polygon(pts))
                if geom.geom_type == "MultiPolygon":
                    geom = max(geom.geoms, key=lambda g: g.area)
                if geom.geom_type != "Polygon" or geom.is_empty:
                    continue
                pts = [(x, y) for x, y in geom.exterior.coords]
                if len(pts) > 1 and pts[0] == pts[-1]:
                    pts.pop()
                pts = merge_collinear(pts, MERGE_EPS * 1.5)
                if len(pts) < 4 or not is_simple(pts):
                    continue
            if len(pts) < 4:
                continue
            a = 0.0
            for i in range(len(pts)):
                x1, y1 = pts[i]
                x2, y2 = pts[(i + 1) % len(pts)]
                a += x1 * y2 - x2 * y1
            if abs(a) / 2.0 < MIN_AREA_SQ_DEG:
                continue
            provinces.append({"ring": pts, "name": f["name"],
                              "nation": f["nation"]})

    n = len(provinces)
    # 统一清尖刺:is_simple 检不出 A->B->A 折返尖刺(make_valid 的
    # 遗留物),耳切遇折返必死锁 → 放射扇形残缺。buffer(0) 经典去刺。
    clean = []
    for p in provinces:
        g = Polygon(p["ring"]).buffer(0)
        if g.is_empty:
            continue
        if g.geom_type == "MultiPolygon":
            g = max(g.geoms, key=lambda x: x.area)
        if g.geom_type != "Polygon":
            continue
        ring = [(x, y) for x, y in g.exterior.coords]
        if len(ring) > 1 and ring[0] == ring[-1]:
            ring.pop()
        ring = merge_collinear(ring, MERGE_EPS)
        if len(ring) >= 4:
            p["ring"] = ring
            clean.append(p)
    provinces = clean
    n = len(provinces)
    print(f"provinces={n} (尖刺清理后)")

    bad = [i for i, p in enumerate(provinces)
        if not is_simple(p["ring"])]
    if bad:
        print(f"drop {len(bad)} self-intersecting rings: {bad[:8]}")
        provinces = [p for i, p in enumerate(provinces)
            if i not in set(bad)]
        n = len(provinces)
    print(f"provinces={n}")
    vx = sorted((len(p["ring"]) for p in provinces), reverse=True)[:5]
    print(f"最大环顶点数 top5: {vx}")

    centroids = []
    for p in provinces:
        xs = [pt[0] for pt in p["ring"]]
        ys = [pt[1] for pt in p["ring"]]
        centroids.append((sum(xs) / len(xs), sum(ys) / len(ys)))

    # ---- 4. 邻接 = 共享边(精确,无需 epsilon) ----
    # 重建后顶点仍逐点一致:共享边的两个端点 key 在两环中都出现。
    # 用边表 users → 环归属:重新扫一遍各省环建立 key→省份集合。
    owner_of_edge = {}
    for pi, p in enumerate(provinces):
        ring = p["ring"]
        for i in range(len(ring)):
            a, b = ring[i], ring[(i + 1) % len(ring)]
            ek = (a, b) if a <= b else (b, a)
            owner_of_edge.setdefault(ek, set()).add(pi)
    adj = [set() for _ in range(n)]
    for users in owner_of_edge.values():
        if len(users) >= 2:
            ul = list(users)
            for u in ul:
                adj[u].update(x for x in ul if x != u)

    # ---- 5. 连通分量/海路 ----
    comp = [-1] * n
    comps = []
    for i in range(n):
        if comp[i] >= 0:
            continue
        stack = [i]
        comp[i] = len(comps)
        cur = [i]
        while stack:
            c = stack.pop()
            for nb in adj[c]:
                if comp[nb] < 0:
                    comp[nb] = comp[i]
                    cur.append(nb)
                    stack.append(nb)
        comps.append(cur)
    comps.sort(key=len, reverse=True)
    sea = 0
    for ci in range(1, len(comps)):
        best = None
        for i in comps[ci]:
            for j in comps[0]:
                d2 = ((centroids[i][0] - centroids[j][0]) ** 2
                    + (centroids[i][1] - centroids[j][1]) ** 2)
                if best is None or d2 < best[0]:
                    best = (d2, i, j)
        if best:
            adj[best[1]].add(best[2])
            adj[best[2]].add(best[1])
            sea += 1
    print(f"components={len(comps)} sea_links={sea}")

    # ---- 6. 地形与输出(mapfile v2 格式) ----
    out = bytearray(b"MAP")
    out += struct.pack("<H", 2)
    out += struct.pack("<H", n)

    def i32(v):
        return struct.pack("<i", int(round(v)))

    terr = []
    for i, p in enumerate(provinces):
        cx, cy = centroids[i]
        out += i32(cx * 1000)
        out += i32(-cy * 1000)
        ring = p["ring"]
        out += struct.pack("<H", len(ring))
        for pt in ring:
            out += i32(pt[0] * 1000)
            out += i32(-pt[1] * 1000)
        terr.append(2 if abs(cy) > MOUNTAIN_LAT
            else (1 if abs(cy) < FOREST_LOW_LAT else 0))
    for t in terr:
        out += struct.pack("<B", t)
    for i in range(n):
        lst = sorted(adj[i])
        out += struct.pack("<H", len(lst))
        for j in lst:
            out += struct.pack("<H", j)

    open(DST, "wb").write(bytes(out))
    print(f"saved {DST}: {n} provinces, terrain="
          f"{[terr.count(t) for t in (0,1,2)]}, {len(out)} bytes")
    # 顺带导出 nation 对照(Phase 2 国家归属用)
    with open(DST + ".nations.txt", "w") as f:
        for i, p in enumerate(provinces):
            f.write(f"{i}\t{p['nation']}\t{p['name']}\n")


if __name__ == "__main__":
    main()
