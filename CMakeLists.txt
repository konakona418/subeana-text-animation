cmake_minimum_required(VERSION 3.10.0)
project(text-animation VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(GRAPHICS "GRAPHICS_API_OPENGL_43")
add_subdirectory(vendors/raylib)
add_subdirectory(vendors/box2d)

add_executable(text-animation main.c slug.cc)
target_link_libraries(text-animation PRIVATE raylib box2d)
target_include_directories(text-animation PRIVATE ./)
