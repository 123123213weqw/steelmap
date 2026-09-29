cmake_minimum_required(VERSION 3.16)
project(steelmap CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ---- 渲染栈(仅 client;无头服务器用 -DSTEELMAP_RENDER=OFF 跳过)----
option(STEELMAP_RENDER "Build SDL3/ImGui render client" ON)
if(STEELMAP_RENDER)
# ---- vendored SDL3(仅 client 链接,core/server 永不触碰)----
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/SDL EXCLUDE_FROM_ALL)

# ---- vendored Dear ImGui(SDL3 + SDL_Renderer 后端)----
add_library(imgui STATIC
	vendor/imgui/imgui.cpp
	vendor/imgui/imgui_draw.cpp
	vendor/imgui/imgui_tables.cpp
	vendor/imgui/imgui_widgets.cpp
	vendor/imgui/backends/imgui_impl_sdl3.cpp
	vendor/imgui/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
endif()

# 纯仿真核心:不 include 任何 OS 头文件,可无头运行
add_library(gs_core STATIC
	core/state.cpp
	core/command.cpp
	core/tick.cpp
	core/serialize.cpp
	core/mapfile.cpp
)

# 传输层:Transport 抽象 + 环回 + XRT 实现(vendored 单头,C 编译)
add_library(gs_net STATIC
	net/loopback.cpp
	net/xrt_transport.cpp
	net/xrt_impl.c
)
target_include_directories(gs_net PUBLIC vendor/xrt)
target_compile_definitions(gs_net PRIVATE XRT_MODULE_ALL)
target_link_libraries(gs_net PUBLIC Threads::Threads m)

enable_testing()

add_executable(determinism_test tests/determinism_test.cpp)
target_link_libraries(determinism_test gs_core gs_net)
add_test(NAME determinism COMMAND determinism_test)

# headless 仿真运行器:兼作回放工具与未来 server 的核心循环雏形
add_executable(sim_run tools/sim_run.cpp)
target_link_libraries(sim_run gs_core gs_net)

# 地图生成器:Voronoi,产出地图内容文件
add_executable(mapgen tools/mapgen.cpp)
target_link_libraries(mapgen gs_core)

# M0:真实两进程
add_executable(server server/main.cpp)
target_link_libraries(server gs_core gs_net)

if(STEELMAP_RENDER)
add_executable(client client/main.cpp client/render.cpp)
target_link_libraries(client gs_core gs_net SDL3::SDL3 imgui)
else()
add_executable(client client/main.cpp client/render_stub.cpp)
target_link_libraries(client gs_core gs_net)
endif()
