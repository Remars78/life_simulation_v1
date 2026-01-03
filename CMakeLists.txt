cmake_minimum_required(VERSION 3.11)
project(ALifeSim)

set(CMAKE_CXX_STANDARD 17)

# --- Raylib Fetch ---
include(FetchContent)
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/master.tar.gz
)
FetchContent_MakeAvailable(raylib)

# --- Android Configuration ---
if(ANDROID)
    add_library(main SHARED src/main.cpp)
    
    # Линковка
    target_link_libraries(main PRIVATE raylib android log EGL GLESv2 OpenSLES)
    
    # Важно: ANativeActivity требует, чтобы библиотека не выгружалась при сворачивании
    target_compile_definitions(main PRIVATE PLATFORM_ANDROID)
else()
    # Для сборки на ПК (тестирование)
    add_executable(ALifeSim src/main.cpp)
    target_link_libraries(ALifeSim PRIVATE raylib)
endif()

