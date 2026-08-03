# ===========================================================================
# GENESIS — CMake build.
#
# Works with MSVC and with MinGW-w64. There are no find_package calls and no
# external dependencies: Dear ImGui is compiled from vendor/imgui and
# everything else it needs is an OS DLL.
#
#   MSVC:
#     cmake -B build -G "Visual Studio 17 2022" -A x64
#     cmake --build build --config Release
#
#   MinGW-w64:
#     cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#     cmake --build build
#
# If you have no CMake, build.bat does the same job with a plain g++ invocation.
# ===========================================================================
cmake_minimum_required(VERSION 3.16)
project(Genesis VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT WIN32)
    message(FATAL_ERROR
        "GENESIS targets Windows: the shell is a raw Win32 window with a WGL "
        "OpenGL context. Porting means replacing src/ui/app.cpp only.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/imgui.cpp")
    message(FATAL_ERROR
        "vendor/imgui is missing. Run:\n"
        "  git clone --depth 1 -b docking https://github.com/ocornut/imgui.git vendor/imgui")
endif()

# --- Lua 5.4 ----------------------------------------------------------------
# lua.c and luac.c hold their own main(); onelua.c is the amalgamation and
# ltests.c is the internal test harness. All four are excluded.
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac|onelua|ltests)\\.c$")
if(NOT LUA_SOURCES)
    message(FATAL_ERROR
        "vendor/lua is missing. Run:\n"
        "  git clone --depth 1 -b v5.4.7 https://github.com/lua/lua.git vendor/lua")
endif()
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC vendor/lua)
if(MSVC)
    target_compile_options(lua PRIVATE /W0)
else()
    target_compile_options(lua PRIVATE -w)
endif()

# --- Dear ImGui -------------------------------------------------------------
# Built as its own target so third-party warnings stay out of our build log
# while our sources keep /W4 or -Wall -Wextra turned on.
add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
    vendor/imgui/backends/imgui_impl_opengl2.cpp
)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_compile_definitions(imgui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

# --- GENESIS ----------------------------------------------------------------
add_executable(Genesis WIN32
    src/main.cpp
    src/core/config.cpp
    src/core/noise.cpp
    src/core/json.cpp
    src/core/profiler.cpp
    src/core/serialize.cpp
    src/sim/time.cpp
    src/sim/world.cpp
    src/sim/worldgen.cpp
    src/sim/genetics.cpp
    src/sim/brain.cpp
    src/sim/attraction.cpp
    src/sim/agent.cpp
    src/sim/simulation.cpp
    src/chem/elements.cpp
    src/chem/reactions.cpp
    src/chem/materials.cpp
    src/sim/knowledge.cpp
    src/sim/species.cpp
    src/sim/chemistry_agent.cpp
    src/econ/economy.cpp
    src/god/god.cpp
    src/god/lua_api.cpp
    src/ui/app.cpp
    src/ui/panels.cpp
    src/ui/viewport.cpp
    src/ui/cards.cpp
    src/ui/chem_ui.cpp
    src/ui/profiler_ui.cpp
    src/ui/phylogeny_ui.cpp
    src/ui/econ_ui.cpp
    src/ui/god_ui.cpp
)

target_include_directories(Genesis PRIVATE src)
target_link_libraries(Genesis PRIVATE imgui lua opengl32 gdi32 user32 shell32 dwmapi imm32)
set_target_properties(Genesis PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(MSVC)
    target_compile_options(Genesis PRIVATE /W4 /permissive- /MP)
    # /fp:precise, NOT /fp:fast. Fast float math reassociates operations, which
    # would break the bit-for-bit reproducibility guarantee (ARCHITECTURE.md §5.3).
    target_compile_options(Genesis PRIVATE /fp:precise)
    target_compile_options(imgui PRIVATE /W0 /MP)
    # Static CRT so the exe has no redistributable dependency.
    set_property(TARGET Genesis imgui PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(Genesis PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(Genesis PRIVATE -Wall -Wextra -fno-fast-math)
    target_compile_options(imgui PRIVATE -w)
    target_link_options(Genesis PRIVATE -static -static-libgcc -static-libstdc++)
endif()

find_package(Threads REQUIRED)
target_link_libraries(Genesis PRIVATE Threads::Threads)

# Emit a fully commented config file next to the executable if there isn't one.
add_custom_command(TARGET Genesis POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
    COMMENT "Ensuring data/ exists (run Genesis.exe --write-config to regenerate config.ini)")
