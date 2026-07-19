# edgehost — multi-plugin io_uring webserver (program design Track 1)
# P1.3: YAML load + SIGHUP shadow apply + ADR-005.

cmake_minimum_required(VERSION 3.20)
project(edgehost LANGUAGES C VERSION 0.3.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # liburing / Linux later

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EdgeDepsCommon.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EdgeLoadPins.cmake")

option(EDGEHOST_REQUIRE_SIBLINGS
  "Fail configure if phase-1 sibling headers are missing" ON)
option(EDGEHOST_REQUIRE_LIBSIM
  "Fail configure if libsim is missing (soft until class-A fuzz)" OFF)

# --- Phase-1 composition roots ---
if(EDGEHOST_REQUIRE_SIBLINGS)
  find_package(Shaggy REQUIRED)
  find_package(Libyaml REQUIRED)
  find_package(Librest REQUIRED)
else()
  find_package(Shaggy)
  find_package(Libyaml)
  find_package(Librest)
endif()

find_package(Pique QUIET)
find_package(Libharness QUIET)
find_package(Libslack QUIET)
find_package(Libteams QUIET)
find_package(Libwebmap QUIET)
if(EDGEHOST_REQUIRE_LIBSIM)
  find_package(Libsim REQUIRED)
else()
  find_package(Libsim QUIET)
endif()

message(STATUS "edgehost dependency map:")
message(STATUS "  SIBLING_ROOT=${SIBLING_ROOT}")
message(STATUS "  shaggy     FOUND=${Shaggy_FOUND} ROOT=${SHAGGY_ROOT}")
message(STATUS "  libyaml    FOUND=${Libyaml_FOUND} ROOT=${LIBYAML_ROOT}")
message(STATUS "  librest    FOUND=${Librest_FOUND} ROOT=${LIBREST_ROOT}")
message(STATUS "  pique      FOUND=${Pique_FOUND} ROOT=${PIQUE_ROOT}")
message(STATUS "  libharness FOUND=${Libharness_FOUND} ROOT=${LIBHARNESS_ROOT}")
message(STATUS "  libslack   FOUND=${Libslack_FOUND} ROOT=${LIBSLACK_ROOT}")
message(STATUS "  libteams   FOUND=${Libteams_FOUND} ROOT=${LIBTEAMS_ROOT}")
message(STATUS "  libwebmap  FOUND=${Libwebmap_FOUND} ROOT=${LIBWEBMAP_ROOT}")
message(STATUS "  libsim     FOUND=${Libsim_FOUND} ROOT=${LIBSIM_ROOT}")

if(Libyaml_FOUND AND NOT TARGET Libyaml::yaml AND NOT LIBYAML_LIBRARY)
  message(WARNING "libyaml headers found but no build/libyaml.a — "
    "config tests need: cmake -B ${LIBYAML_ROOT}/build -S ${LIBYAML_ROOT} && cmake --build ${LIBYAML_ROOT}/build")
endif()

# --- libedgecore (syscall-free) ---
add_library(edgecore STATIC
  src/core/edgecore.c
  src/core/edge_config.c
)
target_include_directories(edgecore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# --- Host pieces: alloc + YAML config + HUP ---
add_library(edgehost_alloc STATIC
  src/host/host_alloc.c
)
target_include_directories(edgehost_alloc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

add_library(edgehost_config STATIC
  src/host/config_yaml.c
  src/host/config_hup.c
)
target_include_directories(edgehost_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(edgehost_config PUBLIC edgecore)
if(TARGET Libyaml::yaml)
  target_link_libraries(edgehost_config PUBLIC Libyaml::yaml)
elseif(LIBYAML_LIBRARY)
  target_link_libraries(edgehost_config PUBLIC ${LIBYAML_LIBRARY})
endif()
if(Libyaml_FOUND AND TARGET Libyaml::headers)
  target_link_libraries(edgehost_config PUBLIC Libyaml::headers)
endif()

# Interface target for sibling headers (deps smoke).
add_library(edgehost_deps INTERFACE)
if(Shaggy_FOUND)
  target_link_libraries(edgehost_deps INTERFACE Shaggy::headers)
endif()
if(Libyaml_FOUND)
  target_link_libraries(edgehost_deps INTERFACE Libyaml::headers)
endif()
if(Librest_FOUND)
  target_link_libraries(edgehost_deps INTERFACE Librest::headers)
endif()

# --- Tests ---
add_executable(edgehost_deps_smoke tests/deps_smoke.c)
target_link_libraries(edgehost_deps_smoke PRIVATE edgehost_deps)
if(Shaggy_FOUND)
  target_compile_definitions(edgehost_deps_smoke PRIVATE EDGEHOST_HAVE_SHAGGY=1)
endif()
if(Libyaml_FOUND)
  target_compile_definitions(edgehost_deps_smoke PRIVATE EDGEHOST_HAVE_LIBYAML=1)
endif()
if(Librest_FOUND)
  target_compile_definitions(edgehost_deps_smoke PRIVATE EDGEHOST_HAVE_LIBREST=1)
endif()

add_executable(edgecore_smoke tests/test_edgecore_smoke.c)
target_link_libraries(edgecore_smoke PRIVATE edgecore)

add_executable(edgecore_alloc_test tests/test_edgecore_alloc.c)
target_link_libraries(edgecore_alloc_test PRIVATE edgecore edgehost_alloc)

add_executable(edgecore_config_test tests/test_edgecore_config.c)
target_link_libraries(edgecore_config_test PRIVATE edgecore edgehost_config)

enable_testing()
add_test(NAME edgehost_deps_smoke COMMAND edgehost_deps_smoke)
add_test(NAME edgecore_smoke COMMAND edgecore_smoke)
add_test(NAME edgecore_alloc COMMAND edgecore_alloc_test)
add_test(NAME edgecore_config COMMAND edgecore_config_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

find_program(EDGEHOST_BASH bash)
if(EDGEHOST_BASH)
  add_test(
    NAME edgehost_verify_pins
    COMMAND ${EDGEHOST_BASH} "${CMAKE_CURRENT_SOURCE_DIR}/deps/verify_pins.sh" --allow-missing
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()

include(GNUInstallDirs)
install(TARGETS edgecore edgehost_alloc edgehost_config
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
install(DIRECTORY cmake/
  DESTINATION lib/cmake/edgehost
  FILES_MATCHING PATTERN "Find*.cmake" PATTERN "Edge*.cmake"
)
install(FILES deps/pins.txt DESTINATION share/edgehost)
install(FILES config/edgehost.example.yaml DESTINATION share/edgehost/config)
