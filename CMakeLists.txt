cmake_minimum_required(VERSION 3.11)
project(ALifeSim)

set(CMAKE_CXX_STANDARD 17)

# Настройка для Android
if(ANDROID)
    set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
    set(CMAKE_ANDROID_STL_TYPE c++_static)
endif()

# Подключаем Raylib через FetchContent
include(FetchContent)
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/master.tar.gz
)
FetchContent_MakeAvailable(raylib)

# Определяем исполняемый файл
# Для Android Raylib требует, чтобы это была библиотека, но современные инструменты позволяют и executable
# Самый надежный способ для Android Native Activity:
add_library(main SHARED src/main.cpp)

# Линкуем Raylib и системные библиотеки Android
target_link_libraries(main PRIVATE raylib android log)

# Копируем ресурсы (если есть) - здесь их нет, все процедурное
