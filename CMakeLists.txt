cmake_minimum_required(VERSION 3.20)
# Dedupe static libs on the link line (Apple ld warns).
if(POLICY CMP0156)
  cmake_policy(SET CMP0156 NEW)
endif()
project(botched LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BOTCHED_BUILD_TESTS "Build unit tests" ON)
option(BOTCHED_BUILD_APPS "Build the headless server host and reference simulator executables" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# --- Deterministic rules library: no platform, rendering or network dependencies.
add_library(botched_core STATIC
  src/types.cpp
  src/json.cpp
  src/map.cpp
  src/state.cpp
  src/engine.cpp
  src/serialize.cpp
  src/validate.cpp
  src/match.cpp
)
target_include_directories(botched_core PUBLIC include)

# --- Portable, allocation-free payload codec. No transport or engine dependencies.
add_library(botched_wire STATIC wire/v1.c)
target_include_directories(botched_wire PUBLIC include)
set_target_properties(botched_wire PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)

# Portable cooperative NIO client; the service boundary is injectable for deterministic tests.
add_library(botched_nio STATIC client/nio.c)
target_link_libraries(botched_nio PUBLIC botched_wire)
set_target_properties(botched_nio PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
# Optional concrete platform binding, supplied by an existing fujinet-nio-lib archive.
set(BOTCHED_NIO_INCLUDE "" CACHE PATH "Directory containing fujinet-nio.h")
set(BOTCHED_NIO_LIBRARY "" CACHE FILEPATH "Platform fujinet-nio-lib archive")
set(BOTCHED_NIO_EXECUTABLE "" CACHE FILEPATH "Existing POSIX FujiNet-NIO TCP executable for opt-in real acceptance")
if(BOTCHED_NIO_INCLUDE AND BOTCHED_NIO_LIBRARY)
  add_library(botched_nio_fujinet STATIC client/nio_fujinet.c)
  target_include_directories(botched_nio_fujinet PRIVATE ${BOTCHED_NIO_INCLUDE})
  target_link_libraries(botched_nio_fujinet PUBLIC botched_nio ${BOTCHED_NIO_LIBRARY})
  set_target_properties(botched_nio_fujinet PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
endif()

# --- Server orchestration (rooms, sessions, API dispatch). Transport-agnostic.
add_library(botched_server STATIC
  server/server.cpp
  server/pool_config.cpp
  server/bot.cpp
  server/map_loader.cpp
  apps/ascii_render.cpp   # board rendering for --trace
)
target_include_directories(botched_server PUBLIC server apps)
target_link_libraries(botched_server PUBLIC botched_core)
# Last-resort default for the maps directory (the source tree); executables first look next to themselves.
target_compile_definitions(botched_server PRIVATE BOTCHED_DEFAULT_MAPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/maps")

add_library(botched_binary STATIC server/binary_codec.cpp server/binary_bridge.cpp)
target_link_libraries(botched_binary PUBLIC botched_server botched_wire)

# --- POSIX reference apps: simulated players and the stdio server host.
add_library(botched_sim_lib STATIC
  apps/sim_runner.cpp
)
target_include_directories(botched_sim_lib PUBLIC apps)
target_link_libraries(botched_sim_lib PUBLIC botched_server)

# --- Minimal POSIX multiplayer MVP (M1-A): TCP host + terminal client. POSIX sockets only.
if(UNIX)
  add_library(botched_tcp_host STATIC server/tcp_host.cpp server/binary_tcp_host.cpp)
  target_include_directories(botched_tcp_host PUBLIC server)
  target_link_libraries(botched_tcp_host PUBLIC botched_server botched_binary)

  add_library(botched_client_lib STATIC apps/client_core.cpp apps/binary_connection.cpp)
  target_include_directories(botched_client_lib PUBLIC apps server)
  target_link_libraries(botched_client_lib PUBLIC botched_sim_lib botched_binary)
endif()

# --- Optional SDL2 graphical client (`botched-client --ui`). Built when SDL2 + SDL2_ttf are found.
option(BOTCHED_SDL "Build the SDL2 graphical client" ON)
set(BOTCHED_HAVE_SDL OFF)
if(UNIX AND BOTCHED_SDL)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(BOTCHED_SDL2 IMPORTED_TARGET sdl2 SDL2_ttf)
  endif()
  if(BOTCHED_SDL2_FOUND)
    set(BOTCHED_HAVE_SDL ON)
  else()
    message(STATUS "SDL2/SDL2_ttf not found: botched-client --ui will be unavailable")
  endif()
endif()
add_library(botched_ui_model STATIC apps/ui_model.cpp apps/ui_playback.cpp)
target_include_directories(botched_ui_model PUBLIC apps)
target_compile_features(botched_ui_model PUBLIC cxx_std_20)
target_link_libraries(botched_ui_model PUBLIC botched_core)
if(BOTCHED_HAVE_SDL)
  add_library(botched_sdl_ui STATIC apps/sdl_ui.cpp)
  target_include_directories(botched_sdl_ui PUBLIC apps)
  target_link_libraries(botched_sdl_ui PUBLIC botched_client_lib botched_ui_model PkgConfig::BOTCHED_SDL2)
endif()

if(BOTCHED_BUILD_APPS)
  add_executable(botched-server apps/botched_server_main.cpp)
  target_link_libraries(botched-server PRIVATE botched_server)

  add_executable(botched-sim apps/botched_sim.cpp)
  target_link_libraries(botched-sim PRIVATE botched_sim_lib)

  if(UNIX)
    add_executable(botched-tcp-server apps/botched_tcpd_main.cpp)
    target_link_libraries(botched-tcp-server PRIVATE botched_tcp_host)
    add_executable(botched-client apps/botched_client_main.cpp)
    target_link_libraries(botched-client PRIVATE botched_client_lib)
    if(BOTCHED_HAVE_SDL)
      target_link_libraries(botched-client PRIVATE botched_sdl_ui)
      target_compile_definitions(botched-client PRIVATE BOTCHED_HAVE_SDL=1)
    endif()
  endif()
endif()

# --- Install: <prefix>/bin/<executables> and <prefix>/share/botched/maps/*.map. The servers find the maps
# next to themselves (../share/botched/maps), so an installed tree works from any working directory.
if(BOTCHED_BUILD_APPS)
  install(TARGETS botched-server botched-sim RUNTIME DESTINATION bin)
  if(UNIX)
    install(TARGETS botched-tcp-server botched-client RUNTIME DESTINATION bin)
  endif()
endif()
install(DIRECTORY resources/maps DESTINATION share/botched FILES_MATCHING PATTERN "*.map")

if(BOTCHED_BUILD_TESTS)
  enable_testing()
  file(GLOB BOTCHED_TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
  add_executable(botched_tests tests/test_main.cpp ${BOTCHED_TEST_SOURCES})
  target_include_directories(botched_tests PRIVATE tests tests/third_party)
  target_compile_definitions(botched_tests PRIVATE BOTCHED_WIRE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/docs/binary-v1/fixtures.json")
  target_compile_definitions(botched_tests PRIVATE BOTCHED_TEST_MAPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/maps")
  target_link_libraries(botched_tests PRIVATE botched_sim_lib botched_ui_model botched_binary botched_nio)
  if(UNIX)
    target_link_libraries(botched_tests PRIVATE botched_tcp_host botched_client_lib)
  endif()
  add_test(NAME botched_tests COMMAND botched_tests)
  if(TARGET botched_nio_fujinet AND UNIX)
    add_executable(botched_nio_real tests/nio_real.cpp)
    target_include_directories(botched_nio_real PRIVATE tests ${BOTCHED_NIO_INCLUDE})
    target_link_libraries(botched_nio_real PRIVATE botched_nio_fujinet botched_tcp_host botched_client_lib)
    if(BOTCHED_NIO_EXECUTABLE)
      find_package(Python3 COMPONENTS Interpreter REQUIRED)
      add_test(NAME botched_nio_real COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/nio_accept.py --nio ${BOTCHED_NIO_EXECUTABLE}
        --check $<TARGET_FILE:botched_nio_real> --drop-read --drop-write)
      set_tests_properties(botched_nio_real PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    endif()
  endif()
  add_executable(botched_wire_c_test tests/wire_c_test.c)
  set_target_properties(botched_wire_c_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_link_libraries(botched_wire_c_test PRIVATE botched_wire)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(botched_wire_c_test PRIVATE -Werror -pedantic-errors -Wvla)
    if(UNIX AND NOT APPLE)
      target_compile_definitions(botched_wire_c_test PRIVATE BW_WRAP_ALLOC=1)
      target_link_options(botched_wire_c_test PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
    endif()
  endif()
  add_test(NAME botched_wire_c_test COMMAND botched_wire_c_test)
  add_executable(botched_nio_c_test tests/nio_c_test.c)
  target_link_libraries(botched_nio_c_test PRIVATE botched_nio)
  set_target_properties(botched_nio_c_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(botched_nio_c_test PRIVATE -Werror -pedantic-errors -Wvla)
    if(UNIX AND NOT APPLE)
      target_compile_definitions(botched_nio_c_test PRIVATE BW_WRAP_ALLOC=1)
      target_link_options(botched_nio_c_test PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
    endif()
  endif()
  add_test(NAME botched_nio_c_test COMMAND botched_nio_c_test)
  if(TARGET botched_nio_fujinet)
    add_executable(botched_nio_binding_test tests/nio_binding_test.c client/nio_fujinet.c)
    target_include_directories(botched_nio_binding_test PRIVATE include ${BOTCHED_NIO_INCLUDE})
    set_target_properties(botched_nio_binding_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    add_test(NAME botched_nio_binding_test COMMAND botched_nio_binding_test)
  endif()
  if(BOTCHED_HAVE_SDL AND BOTCHED_BUILD_APPS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME botched_binary_cli COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/binary_tcp_cli.py
        $<TARGET_FILE:botched-tcp-server> $<TARGET_FILE:botched-client>)
      set_tests_properties(botched_binary_cli PROPERTIES TIMEOUT 60)
    endif()
  endif()
endif()

# Opt-in measured 6502 acceptance; requires the existing Atari general NIO archive.
option(BOTCHED_B5_MEASURE "Run cc65/sim65 constrained-client measurements" OFF)
if(BOTCHED_BUILD_TESTS AND BOTCHED_B5_MEASURE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_program(BOTCHED_CL65 cl65 REQUIRED)
  find_program(BOTCHED_SIM65 sim65 REQUIRED)
  add_test(NAME botched_b5_resources COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/b5/measure.py --out ${CMAKE_CURRENT_BINARY_DIR}/b5)
  set_tests_properties(botched_b5_resources PROPERTIES TIMEOUT 180)
endif()

# Review-only thin Atari feasibility prototype, isolated from production clients.
option(BOTCHED_ATARI_SPIKE "Build thin Atari transport/decoder feasibility tests" OFF)
if(BOTCHED_ATARI_SPIKE)
  if(NOT EXISTS "${BOTCHED_NIO_INCLUDE}/fn_stream.h")
    message(FATAL_ERROR "Set BOTCHED_NIO_INCLUDE to the existing fujinet-nio-lib include directory")
  endif()
  add_library(botched_atari_thin STATIC prototypes/atari/thin.c prototypes/atari/stream.c)
  target_include_directories(botched_atari_thin PUBLIC prototypes/atari ${BOTCHED_NIO_INCLUDE})
  set_target_properties(botched_atari_thin PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  add_executable(botched_atari_spike tests/atari-spike/host.cpp)
  target_link_libraries(botched_atari_spike PRIVATE botched_atari_thin botched_binary botched_ui_model)
  if(BOTCHED_BUILD_TESTS)
    add_test(NAME botched_atari_spike COMMAND botched_atari_spike ${CMAKE_CURRENT_BINARY_DIR}/atari-spike/corpus)
  endif()
endif()

if(BOTCHED_BUILD_TESTS)
  add_library(botched_amiga_model STATIC client/amiga/model.c client/amiga/events.c client/amiga/play.c client/amiga/session.c client/amiga/assets.c client/amiga/visual.c client/amiga/narration.c client/amiga/view.c)
  target_link_libraries(botched_amiga_model PUBLIC botched_wire botched_nio)
  target_include_directories(botched_amiga_model PUBLIC client/amiga)
  set_target_properties(botched_amiga_model PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
  add_executable(botched_amiga_narration_test tests/amiga/narration.cpp)
  target_link_libraries(botched_amiga_narration_test PRIVATE botched_amiga_model botched_binary)
  add_test(NAME botched_amiga_narration COMMAND botched_amiga_narration_test ${CMAKE_CURRENT_SOURCE_DIR}/docs/binary-v1/fixtures.json)
  add_executable(botched_amiga_view_test tests/amiga/view.cpp)
  target_link_libraries(botched_amiga_view_test PRIVATE botched_amiga_model botched_binary)
  add_test(NAME botched_amiga_view COMMAND botched_amiga_view_test)
  add_executable(botched_amiga_model_test tests/amiga/model.cpp)
  target_link_libraries(botched_amiga_model_test PRIVATE botched_amiga_model botched_binary)
  add_test(NAME botched_amiga_model COMMAND botched_amiga_model_test)
endif()
if(BOTCHED_BUILD_TESTS)
  add_executable(botched_amiga_gameplay_test tests/amiga/gameplay.cpp)
  target_link_libraries(botched_amiga_gameplay_test PRIVATE botched_amiga_model botched_binary botched_ui_model)
  add_test(NAME botched_amiga_gameplay COMMAND botched_amiga_gameplay_test)
endif()

if(BOTCHED_BUILD_TESTS)
  add_executable(botched_amiga_reliability_test tests/amiga/reliability.cpp)
  target_include_directories(botched_amiga_reliability_test PRIVATE tests)
  target_link_libraries(botched_amiga_reliability_test PRIVATE botched_amiga_model botched_binary)
  add_test(NAME botched_amiga_reliability COMMAND botched_amiga_reliability_test)
endif()

if(BOTCHED_BUILD_TESTS AND BOTCHED_BUILD_APPS AND UNIX)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME botched_pool_integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pool_integration.py
    $<TARGET_FILE:botched-tcp-server> $<TARGET_FILE:botched-client>)
  set_tests_properties(botched_pool_integration PROPERTIES TIMEOUT 120)
  add_test(NAME botched_pool_lifecycle_integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pool_lifecycle_integration.py
    $<TARGET_FILE:botched-tcp-server> $<TARGET_FILE:botched-client>)
  set_tests_properties(botched_pool_lifecycle_integration PROPERTIES TIMEOUT 90)
endif()
