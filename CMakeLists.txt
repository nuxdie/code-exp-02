cmake_minimum_required(VERSION 3.16)
project(white_window LANGUAGES CXX)

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TYPOGRAPHY REQUIRED IMPORTED_TARGET pangocairo fontconfig)

foreach(FONT_FILE JetBrainsMono-Regular.ttf JetBrainsMono-Bold.ttf JetBrainsMono-Italic.ttf OFL.txt)
    configure_file("fonts/${FONT_FILE}" "fonts/${FONT_FILE}" COPYONLY)
endforeach()

# Embed the source at build time so the executable can display it anywhere.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/camera.h")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" MAIN_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/camera.h" CAMERA_SOURCE)
set(PROGRAM_SOURCE "// main.cpp\n${MAIN_SOURCE}\n// camera.h\n${CAMERA_SOURCE}")
configure_file(source_text.h.in source_text.h @ONLY)

add_executable(white_window main.cpp)
target_compile_features(white_window PRIVATE cxx_std_17)
target_include_directories(white_window PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(white_window PRIVATE X11::X11 PkgConfig::TYPOGRAPHY)

include(CTest)
if(BUILD_TESTING)
    add_executable(camera_tests tests/camera_test.cpp)
    target_compile_features(camera_tests PRIVATE cxx_std_17)
    target_include_directories(camera_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME camera COMMAND camera_tests)
endif()
