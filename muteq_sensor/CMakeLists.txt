cmake_minimum_required(VERSION 3.25)
project(muteq_daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -Wall -Wextra -Wpedantic 
    -Wshadow -Wconversion -Wnon-virtual-dtor
)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

add_executable(muteq_sensor
    src/main.cpp
)

target_include_directories(muteq_sensor PRIVATE src)
target_link_libraries(muteq_sensor PRIVATE SQLite3::SQLite3 Threads::Threads CURL::libcurl)
