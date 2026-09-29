# steelmap(M0 工作名)

HOI 类大战略游戏。服务器权威仿真 + 本地渲染,纯 C++ 极简依赖。

> **[踩坑记录](docs/PITFALLS.md)**:XRT×7、SDL×6、逻辑×7、工具链×8 全部实录,
> 含根因与预防。改网络/渲染/部署前先过一遍。

## 结构

```
core/     纯 C++ 仿真核心——不 include 任何 OS 头文件,确定性,可无头运行
  state      GameState / 省份图 / 单位
  command    玩家命令:validate 强制校验,apply 内部再校验一次
  tick       stepDay:按单位 id 升序结算,沿最短路每日一跳(BFS)
  serialize  序列化 = 存档 = 网络 = 回放,小端定长头,改格式必须升版本
  rng        xoshiro256**,整数随机,状态随存档走
net/      传输层抽象(Transport 接口),core/server 不直接依赖 XRT
tests/    确定性 / 回放 / 分歧 / 序列化往返 / 移动正确性 / 越权拒绝 / 环回路由
tools/    sim_run:headless 仿真运行器,兼回放工具
vendor/   xrt/xrt.h —— 锁定 gitee xywhsoft/xrt commit 18747e3 (sha256 ebd826df...)
```

## 构建

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/determinism_test          # 必须全绿才能提交
./tools/accept_m0.sh              # 联机验收(自动起 server + 双 client + 回放对账)
```

sim_run 脚本格式:每行 `tick unitId targetProvince`。

## M0 验收状态(2026-09-28,全部通过)

- [x] 同种子 + 同命令流 → 哈希一致(回放免费获得)
- [x] 命令不同 → 哈希不同(分歧可检测)
- [x] 序列化往返无损,往返后推演仍一致
- [x] 越权命令双重拒绝(validate 返回 false;误调 apply 也是无操作)
- [x] **联机**:server + 双 client 三进程,三方最终哈希一致
- [x] **反欺骗**:命令的 player 字段由服务器按连接覆盖,A 动 B 的单位被拒
- [x] **回放对账**:在线对局日志用 sim_run 离线重演,哈希逐位一致
- [x] macOS(Apple clang)与 Linux(gcc 12)同哈希:2399e14ff977ae60

已知噪声:vendor/xrt/xrt.h 在 -Wextra 下有 9 条警告(vendored 代码,非本项目代码)。

## 进程与线程模型(M0 定型)

- server:全部逻辑(命令、tick、广播)在 XRT 单 worker 线程串行,无锁;
  主线程只等待结束
- client:收发与脚本调度在 worker 线程,主线程只等待
- Transport::after() 与消息回调同线程 —— 这是免锁的前提,换传输实现必须保持

## 下一步(M1 剩余)

- [x] 渲染骨架:SDL3 + ImGui + 省份多边形/邻接边/单位/插值动画/点击下令(M1 首张画面已出)
- [ ] 真省份数据:多边形 + 邻接表外置(core 加载器),替换 32 省占位图
- [ ] 单位选中/取消、多单位命令、相机跟随
- [ ] 中文 UI(需要 CJK 字体接入 ImGui)

## M1 真地图管线(已完成)

- `ProvinceGraph` 含多边形几何(定点 milli 坐标),序列化 v2,随快照下发——客户端不需要地图文件
- `core/mapfile`:地图内容文件格式(与存档分离),`server --map` 加载
- `tools/mapgen`:Voronoi 生成器(抖动网格 + 半平面切割 + 共享边邻接),`--grid 8 --seed 7` → 64 省连通地图
- 渲染:真实多边形 + 省界描边 + 点内拾取(even-odd 射线法)替代占位环
- 快照 4KB(原 300B)→ 暴露并修复 XRT 接收 bug(见下)

## XRT 关键教训(已固化在 net/xrt_transport.cpp)

1. Read 回调必须直接消费 pBuffer(xrtNetBufSpans + xrtNetBufConsume),
   不能用 xrtNetStreamRead 拉数据:后者会丢每个完成式读取单元的首块
   (ReadSize=2048)——小消息不可见,大帧必丢。
2. 回调重入:在 worker 线程上调 xrtNetStreamSend,XRT 会同步派发挂起的
   Close 事件 → 回调里 erase streams 会炸掉进行中的迭代。broadcast 必须
   先拍快照再发送(ASan 定位,heap-use-after-free)。
3. 流销毁时机:Close 回调当拍、甚至同拍末尾 Destroy 都会 UAF;隔一个
   timer 节拍销毁(dyingStreams 两段延迟)验证稳定。

## UI 层(已完成:事件日志/顶栏/选中/中文)

- 协议新增 Event 消息:服务器每日对比状态差异生成事件(攻占/全灭/新建/获胜)
- 客户端:左下战报窗口(中文、按玩家着色)、顶部信息栏(天数/补给/收入/部队/领土)
- 单位选中:点自己部队选中(白环),Tab 循环,选中后点任意省下令——新造的兵可指挥了
- 中文字体:加载系统 PingFang(ImGui 1.92 动态字体,按需光栅化)
- 战斗指示:交战省红色脉冲边框;单位旁实时兵力数字;H 帮助窗口
- 悬停 tooltip:单位(兵力/状态)、省份(归属/收入)

## M3 经济与生产(最小版,已完成)

- 补给库存(每玩家):每 own 一省日入 +2,序列化 v4
- 回血:驻己方省日 +2(上限 100)——消耗战核心
- BuildUnit 命令:300 补给在己方省造满编新单位(客户端按 B 键)
- AI 同样攒钱造兵;回放格式扩展 "tick B province" 行
- 幽灵行军修复:死人不动、不能受令(测试 10)

## M2 战斗系统(已完成)

- 省份 owner(中立 0xFFFE)+ 单位 strength,序列化 v3
- stepDay 三段:移动 → 战斗(同城异主对掷 8-15 伤害,整数 RNG)→ 占领
- 服务器 --ai N:机器人玩家(多源 BFS 找最近非己方省),走命令通道,
  回放自动一致
- 胜负:只剩一家有存活单位 → WINNER
- sim_run --map:任意地图回放
- 验收:tools/accept_m2.sh(AI 战争 + 归属上色 + 回放对账)

## 渲染层踩坑记录(SDL 3.4.16,Mac/Metal)

1. `SDL_SetRenderDrawColor` 收 Uint8(0-255),浮点色用 `SDL_SetRenderDrawColorFloat`——传浮点被截 0,清屏全黑
2. `SDL_RenderGeometry` 非索引形式(indices=nullptr)静默失败——始终传索引数组
3. `SDL_RenderReadPixels` 返回 `SDL_Surface*`,需自行 ConvertSurface
4. `SDL_PIXELFORMAT_RGBA8888` 内存字节序是 A,B,G,R(小端),转 BMP 时通道别搞反
5. 无纹理(nullptr)几何体在 Metal 后端不可靠——学 ImGui,统一绑 1×1 白纹理

## 联机备忘

- 服务器不等慢客户端;房主控速,任何人可暂停(M4)
- 快照生成带 viewer 参数(现在全图可见,战争迷雾只换过滤器)
- 每房间绑一个 XRT worker,回调免锁(M4 房间管理器)
