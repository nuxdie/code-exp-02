cmake_minimum_required(VERSION 3.16)
project(white_window LANGUAGES CXX)

find_package(X11 REQUIRED)

add_executable(white_window main.cpp)
target_compile_features(white_window PRIVATE cxx_std_17)
target_link_libraries(white_window PRIVATE X11::X11)
