cmake_minimum_required(VERSION 3.20)

# AGENT-CTX: engine is a static library that grows slice by slice.
# Slice 1 — stub only. No game logic yet. Server and tests link against this target.
# Never add network or I/O code here; that belongs in server/.
add_library(engine STATIC
    src/engine.cpp
)

target_include_directories(engine PUBLIC include)
target_compile_features(engine PUBLIC cxx_std_17)
