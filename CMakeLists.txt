cmake_minimum_required(VERSION 3.10.0)
project(text-animation VERSION 0.1.0 LANGUAGES C)

add_subdirectory(vendors/raylib)
add_subdirectory(vendors/box2d)

add_executable(text-animation main.c)
target_link_libraries(text-animation PRIVATE raylib box2d)
