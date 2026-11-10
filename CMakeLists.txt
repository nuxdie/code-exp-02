cmake_minimum_required(VERSION 3.16)
project(white_window LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(
    TYPOGRAPHY
    REQUIRED
    IMPORTED_TARGET
    pangocairo
    fontconfig
    librsvg-2.0
    gio-2.0
)
pkg_check_modules(GPU REQUIRED IMPORTED_TARGET epoxy)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
option(ENABLE_CLANG_TIDY "Enforce static analysis when compiling C++" ON)
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy-18 clang-tidy REQUIRED)
if(ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};--quiet")
endif()
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy"
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake_style.py" --write
    RESULT_VARIABLE CMAKE_STYLE_RESULT
)
if(NOT CMAKE_STYLE_RESULT EQUAL 0)
    message(FATAL_ERROR "CMake formatting or lint checks failed")
endif()
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/.cmake-format.json"
             "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake_style.py"
)
find_program(
    CLANGD_EXECUTABLE NAMES clangd clangd-21 clangd-20 clangd-19 clangd-18
)
if(NOT CLANGD_EXECUTABLE)
    set(CLANGD_EXECUTABLE clangd)
endif()
find_program(
    CMAKE_LSP_EXECUTABLE
    NAMES neocmakelsp
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/.cache/tools"
)
if(NOT CMAKE_LSP_EXECUTABLE)
    set(CMAKE_LSP_EXECUTABLE neocmakelsp)
endif()
configure_file(viewer_config.h.in viewer_config.h @ONLY)
configure_file(tools/agent_bridge.py agent_bridge.py COPYONLY)
foreach(provenance_file provenance.py provenance_git.py provenance_store.py)
    configure_file("tools/${provenance_file}" "${provenance_file}" COPYONLY)
endforeach()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import websockets"
    RESULT_VARIABLE WEBSOCKETS_RESULT
)
if(NOT WEBSOCKETS_RESULT EQUAL 0)
    message(
        FATAL_ERROR "Python websockets is required (install python3-websockets)"
    )
endif()
file(COPY fonts/noto-serif DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/fonts")
file(COPY assets DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

foreach(font_file JetBrainsMono-Regular.ttf JetBrainsMono-Bold.ttf
                  JetBrainsMono-Italic.ttf OFL.txt
)
    configure_file("fonts/${font_file}" "fonts/${font_file}" COPYONLY)
endforeach()

# Discover project sources for formatting and build dependencies.
file(
    GLOB_RECURSE VIEWER_SOURCES CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "*.cpp"
    "*.h"
    "*.hpp"
    "*.cc"
    "*.cxx"
    "*.c"
    "*.in"
    "*.cmake"
    "*.py"
    "*.yml"
    "CMakeLists.txt"
    ".clang-format"
    ".clang-tidy"
)
list(FILTER VIEWER_SOURCES EXCLUDE REGEX
     "(^|/)(build[^/]*|\\.git|\\.cache|fonts|assets)/"
)
list(REMOVE_ITEM VIEWER_SOURCES main.cpp camera.h canvas.h)
list(PREPEND VIEWER_SOURCES main.cpp camera.h canvas.h)

# Formatting is a required part of build configuration.
find_program(
    CLANG_FORMAT_EXECUTABLE NAMES clang-format-18 clang-format REQUIRED
)
set(FORMAT_SOURCES "")
foreach(source_path IN LISTS VIEWER_SOURCES)
    if(source_path MATCHES "\\.(cpp|cc|cxx|c|h|hpp)$")
        list(APPEND FORMAT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${source_path}")
    endif()
endforeach()
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/.clang-format"
)
execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --style=file -i ${FORMAT_SOURCES}
    RESULT_VARIABLE FORMAT_RESULT
)
if(NOT FORMAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Automatic C++ formatting failed")
endif()
add_custom_target(
    format
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake_style.py" --write
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --style=file -i ${FORMAT_SOURCES}
    COMMENT "Formatting C++ sources"
)
add_custom_target(
    format-check
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake_style.py"
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --style=file --dry-run --Werror
            ${FORMAT_SOURCES}
    COMMENT "Checking C++ formatting"
)

foreach(source_path IN LISTS VIEWER_SOURCES)
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/${source_path}"
    )
endforeach()

add_library(agent_connection agent_client.cpp conversation.cpp text_input.cpp)
target_compile_features(agent_connection PUBLIC cxx_std_17)
target_include_directories(
    agent_connection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
    agent_connection PUBLIC nlohmann_json::nlohmann_json Threads::Threads
)
add_library(live_build live_build.cpp)
target_compile_features(live_build PUBLIC cxx_std_17)
target_include_directories(live_build PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(live_build PUBLIC Threads::Threads)
add_library(project_watch project_watch.cpp)
target_compile_features(project_watch PUBLIC cxx_std_17)
target_include_directories(project_watch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(project_watch PUBLIC Threads::Threads)

add_library(language_server lsp.cpp)
target_compile_features(language_server PUBLIC cxx_std_17)
target_link_libraries(
    language_server PUBLIC nlohmann_json::nlohmann_json Threads::Threads
)
target_include_directories(language_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
find_program(LATEX_EXECUTABLE latex REQUIRED)
find_program(DVISVGM_EXECUTABLE dvisvgm REQUIRED)
add_library(math_typesetter math_typesetter.cpp)
target_compile_features(math_typesetter PUBLIC cxx_std_17)
target_include_directories(math_typesetter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(math_typesetter PUBLIC Threads::Threads)
find_program(NEATO_EXECUTABLE neato REQUIRED)
add_library(source_graph source_graph.cpp)
target_compile_features(source_graph PUBLIC cxx_std_17)
target_include_directories(source_graph PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(source_graph PUBLIC nlohmann_json::nlohmann_json)

add_library(gpu_renderer gpu.cpp)
target_compile_features(gpu_renderer PUBLIC cxx_std_17)
target_include_directories(gpu_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    gpu_renderer PUBLIC X11::X11 PkgConfig::TYPOGRAPHY PkgConfig::GPU
)

add_library(
    application_core
    agent_activity.cpp
    application_agent_activity.cpp
    application.cpp
    application_workspace.cpp
    directory_picker.cpp
    application_model.cpp
    application_navigation.cpp
    application_popups.cpp
    application_search.cpp
    application_search_input.cpp
    application_search_view.cpp
    application_shortcuts.cpp
    application_agent_windows.cpp
    application_agent_context.cpp
    application_agent_menu.cpp
    application_agent_session.cpp
    application_cmake.cpp
    application_file_links.cpp
    application_text_targets.cpp
    application_text_selection.cpp
    application_text_selection_view.cpp
    system_theme.cpp
    intent_document.cpp
    application_intent.cpp
    application_intent_badges.cpp
    application_intent_view.cpp
    application_intent_input.cpp
    application_view.cpp
    application_events.cpp
    application_language.cpp
    application_live.cpp
    application_chat.cpp
    application_chat_scroll.cpp
    application_chat_view.cpp
    application_chat_input.cpp
    application_chat_questions.cpp
    application_composer.cpp
    application_session.cpp
    application_reload.cpp
    application_math.cpp
    application_pointer.cpp
    popup_markup.cpp
    syntax.cpp
    source_panel.cpp
)
target_compile_features(application_core PUBLIC cxx_std_17)
target_include_directories(
    application_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                            "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(
    application_core
    PUBLIC gpu_renderer
           language_server
           source_graph
           math_typesetter
           project_watch
           agent_connection
           live_build
)

add_executable(white_window main.cpp)
target_compile_features(white_window PRIVATE cxx_std_17)
target_include_directories(white_window PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(white_window PRIVATE application_core)

include(CTest)
if(BUILD_TESTING)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    add_executable(agent_activity_tests tests/agent_activity_test.cpp)
    target_link_libraries(agent_activity_tests PRIVATE application_core)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME agent_activity
                 COMMAND "${XVFB_RUN_EXECUTABLE}" -a
                         $<TARGET_FILE:agent_activity_tests>
        )
    else()
        add_test(NAME agent_activity COMMAND agent_activity_tests)
    endif()
    set_tests_properties(agent_activity PROPERTIES TIMEOUT 30)
    add_executable(
        system_theme_tests tests/system_theme_test.cpp system_theme.cpp
    )
    target_include_directories(
        system_theme_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        system_theme_tests PRIVATE PkgConfig::TYPOGRAPHY Threads::Threads
    )
    add_test(NAME system_theme COMMAND system_theme_tests)
    set_tests_properties(system_theme PROPERTIES TIMEOUT 15)
    add_executable(project_watch_tests tests/project_watch_test.cpp)
    target_link_libraries(project_watch_tests PRIVATE project_watch)
    add_test(NAME project_watch COMMAND project_watch_tests)
    add_executable(conversation_tests tests/conversation_test.cpp)
    target_link_libraries(conversation_tests PRIVATE agent_connection)
    add_test(NAME conversation COMMAND conversation_tests)
    add_executable(live_build_tests tests/live_build_test.cpp)
    target_link_libraries(live_build_tests PRIVATE live_build)
    add_test(NAME live_build COMMAND live_build_tests)
    set_tests_properties(live_build PROPERTIES TIMEOUT 30)
    add_executable(agent_live_tests tests/agent_live_test.cpp)
    target_link_libraries(agent_live_tests PRIVATE agent_connection)
    target_compile_definitions(
        agent_live_tests
        PRIVATE TEST_PYTHON="${Python3_EXECUTABLE}"
                TEST_BRIDGE="${CMAKE_CURRENT_BINARY_DIR}/agent_bridge.py"
    )
    add_test(NAME agent_live COMMAND agent_live_tests)
    set_tests_properties(agent_live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
    add_executable(camera_tests tests/camera_test.cpp)
    target_compile_features(camera_tests PRIVATE cxx_std_17)
    target_include_directories(
        camera_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME camera COMMAND camera_tests)
    add_executable(canvas_tests tests/canvas_test.cpp)
    target_compile_features(canvas_tests PRIVATE cxx_std_17)
    target_include_directories(
        canvas_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME canvas COMMAND canvas_tests)
    add_executable(lsp_tests tests/lsp_test.cpp)
    target_link_libraries(lsp_tests PRIVATE language_server)
    target_compile_definitions(
        lsp_tests PRIVATE TEST_CLANGD="${CLANGD_EXECUTABLE}"
    )
    add_test(NAME lsp COMMAND lsp_tests)
    set_tests_properties(lsp PROPERTIES TIMEOUT 30)
    add_executable(gpu_tests tests/gpu_test.cpp)
    target_link_libraries(gpu_tests PRIVATE gpu_renderer)
    target_compile_definitions(
        gpu_tests PRIVATE TEST_FONTS="${CMAKE_CURRENT_BINARY_DIR}/fonts"
    )
    add_test(NAME gpu COMMAND gpu_tests)
    set_tests_properties(gpu PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(structure_tests tests/structure_test.cpp)
    target_link_libraries(structure_tests PRIVATE source_graph language_server)
    add_test(NAME structure COMMAND structure_tests)
    add_executable(syntax_tests tests/syntax_test.cpp syntax.cpp)
    target_compile_features(syntax_tests PRIVATE cxx_std_17)
    target_include_directories(
        syntax_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME syntax COMMAND syntax_tests)
    add_executable(popup_tests tests/popup_test.cpp popup_markup.cpp)
    target_compile_features(popup_tests PRIVATE cxx_std_17)
    target_include_directories(
        popup_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME popup COMMAND popup_tests)
    add_executable(math_tests tests/math_test.cpp)
    target_link_libraries(math_tests PRIVATE math_typesetter)
    add_test(NAME math COMMAND math_tests)
    set_tests_properties(math PROPERTIES TIMEOUT 20)
    add_executable(selection_tests tests/selection_test.cpp)
    target_link_libraries(selection_tests PRIVATE application_core)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME selection COMMAND "${XVFB_RUN_EXECUTABLE}" -a
                                        $<TARGET_FILE:selection_tests>
        )
    else()
        add_test(NAME selection COMMAND selection_tests)
    endif()
    set_tests_properties(selection PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(agent_windows_tests tests/agent_windows_test.cpp)
    add_executable(search_tests tests/search_test.cpp)
    target_link_libraries(search_tests PRIVATE application_core)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME search COMMAND "${XVFB_RUN_EXECUTABLE}" -a
                                     $<TARGET_FILE:search_tests>
        )
    else()
        add_test(NAME search COMMAND search_tests)
    endif()
    set_tests_properties(search PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(navigation_tests tests/navigation_test.cpp)
    target_link_libraries(navigation_tests PRIVATE application_core)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME navigation COMMAND "${XVFB_RUN_EXECUTABLE}" -a
                                         $<TARGET_FILE:navigation_tests>
        )
    else()
        add_test(NAME navigation COMMAND navigation_tests)
    endif()
    set_tests_properties(navigation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    target_link_libraries(agent_windows_tests PRIVATE application_core)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME agent_windows COMMAND "${XVFB_RUN_EXECUTABLE}" -a
                                            $<TARGET_FILE:agent_windows_tests>
        )
    else()
        add_test(NAME agent_windows COMMAND agent_windows_tests)
    endif()
    set_tests_properties(
        agent_windows PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
    )
    add_executable(cmake_canvas_tests tests/cmake_canvas_test.cpp)
    target_link_libraries(cmake_canvas_tests PRIVATE application_core)
    add_test(NAME cmake_canvas COMMAND cmake_canvas_tests)
    set_tests_properties(cmake_canvas PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
    add_executable(intent_canvas_tests tests/intent_canvas_test.cpp)
    target_link_libraries(intent_canvas_tests PRIVATE application_core)
    add_test(NAME intent_canvas COMMAND intent_canvas_tests)
    set_tests_properties(
        intent_canvas PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45
    )
    add_executable(live_canvas_tests tests/live_canvas_test.cpp)
    target_link_libraries(live_canvas_tests PRIVATE application_core)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME live_canvas COMMAND "${XVFB_RUN_EXECUTABLE}" -a
                                          $<TARGET_FILE:live_canvas_tests>
        )
    else()
        add_test(NAME live_canvas COMMAND live_canvas_tests)
    endif()
    set_tests_properties(live_canvas PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 35)
    add_executable(workspace_tests tests/workspace_test.cpp)
    target_link_libraries(workspace_tests PRIVATE application_core)
    add_test(NAME workspace COMMAND workspace_tests)
    set_tests_properties(workspace PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(integration_tests tests/integration_test.cpp)
    target_link_libraries(integration_tests PRIVATE application_core)
    add_test(NAME integration COMMAND integration_tests)
    set(INTEGRATION_ENVIRONMENT
        "SOURCE_CANVAS_DISABLE_AGENT=1" "SOURCE_CANVAS_AUTO_BUILD=0"
        "SOURCE_CANVAS_DISABLE_SESSION=1"
    )
    set_tests_properties(
        integration
        PROPERTIES SKIP_RETURN_CODE
                   77
                   TIMEOUT
                   45
                   ENVIRONMENT
                   "${INTEGRATION_ENVIRONMENT}"
                   WORKING_DIRECTORY
                   "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME agent_bridge
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_agent_bridge.py
    )
    add_test(NAME provenance
             COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_provenance.py
    )
    add_test(NAME cmake_style
             COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_cmake_style.py
    )
    add_test(NAME quality_policy
             COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_quality.py
    )
endif()

add_custom_target(
    quality
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality.py"
        --build-dir "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS white_window
    USES_TERMINAL
    COMMENT "Checking formatting, maintainability, static analysis and tests"
)
if(BUILD_TESTING)
    add_dependencies(
        quality
        agent_activity_tests
        selection_tests
        agent_windows_tests
        search_tests
        navigation_tests
        cmake_canvas_tests
        intent_canvas_tests
        system_theme_tests
        project_watch_tests
        conversation_tests
        live_build_tests
        agent_live_tests
        live_canvas_tests
        workspace_tests
        camera_tests
        canvas_tests
        lsp_tests
        gpu_tests
        structure_tests
        syntax_tests
        popup_tests
        math_tests
        integration_tests
    )
endif()
