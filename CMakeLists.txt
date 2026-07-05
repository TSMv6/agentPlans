cmake_minimum_required(VERSION 3.16)
project(agentPlans LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Statically link the MSVC C/C++ runtime so the distributed exe needs no Visual
# Studio redistributable DLLs (MSVCP140/VCRUNTIME140/…). Self-contained binary.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /W3)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Vendored miniz (C) for gzip read/write — same lib AgentFlow/Hydra uses.
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)

add_library(agentplans_core STATIC
  src/util/csv.cpp
  src/util/gz_io.cpp
  src/settings.cpp
  src/lookups.cpp
  src/stage_ldt.cpp
  src/stage_eltod.cpp
)
target_include_directories(agentplans_core PUBLIC src)
target_link_libraries(agentplans_core PUBLIC miniz)

add_executable(agentPlans src/main.cpp)
target_link_libraries(agentPlans PRIVATE agentplans_core)
