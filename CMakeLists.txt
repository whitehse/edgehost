# edgehost — multi-plugin io_uring webserver (program design Track 1)
# Track 1 complete through P1.15 (extra ns + deploy / Prometheus notes).

cmake_minimum_required(VERSION 3.20)
project(edgehost LANGUAGES C VERSION 0.10.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # liburing / Linux

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EdgeDepsCommon.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EdgeLoadPins.cmake")

option(EDGEHOST_REQUIRE_SIBLINGS
  "Fail configure if phase-1 sibling headers are missing" ON)
option(EDGEHOST_REQUIRE_LIBSIM
  "Fail configure if libsim is missing (needed for P1.5 sim_main)" ON)
option(BUILD_FUZZ
  "Build libFuzzer class-A harness (clang)" OFF)

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
# Optional: E7 Call Home NETCONF SM + Call Home SSH (libchssh preferred).
find_package(Libchssh QUIET)
find_package(Libnetconf QUIET)
find_package(Libyang QUIET)
if(NOT DEFINED EDGEHOST_E7_SSH_AVAILABLE)
  set(EDGEHOST_E7_SSH_AVAILABLE 0)
endif()
if(NOT DEFINED EDGEHOST_E7_CHSSH_AVAILABLE)
  set(EDGEHOST_E7_CHSSH_AVAILABLE 0)
endif()
if(NOT DEFINED EDGEHOST_HAVE_LIBYANG)
  set(EDGEHOST_HAVE_LIBYANG 0)
endif()
if(EDGEHOST_REQUIRE_LIBSIM)
  find_package(Libsim REQUIRED)
else()
  find_package(Libsim QUIET)
endif()

find_library(LIBURING_LIBRARY uring)
find_path(LIBURING_INCLUDE_DIR liburing.h)
if(NOT LIBURING_LIBRARY OR NOT LIBURING_INCLUDE_DIR)
  message(FATAL_ERROR "liburing required (install liburing-dev)")
endif()

find_package(OpenSSL REQUIRED)
message(STATUS "  OpenSSL    ${OPENSSL_VERSION}")

message(STATUS "edgehost dependency map:")
message(STATUS "  SIBLING_ROOT=${SIBLING_ROOT}")
message(STATUS "  shaggy     FOUND=${Shaggy_FOUND} ROOT=${SHAGGY_ROOT} LIB=${SHAGGY_LIBRARY}")
message(STATUS "  libyaml    FOUND=${Libyaml_FOUND} ROOT=${LIBYAML_ROOT}")
message(STATUS "  librest    FOUND=${Librest_FOUND} ROOT=${LIBREST_ROOT}")
message(STATUS "  libsim     FOUND=${Libsim_FOUND} ROOT=${LIBSIM_ROOT} LIB=${LIBSIM_LIBRARY}")
if(Libnetconf_FOUND)
  message(STATUS "  libnetconf FOUND=${Libnetconf_FOUND} ROOT=${LIBNETCONF_ROOT} LIB=${LIBNETCONF_LIBRARY}")
else()
  message(STATUS "  libnetconf FOUND=0 (optional; E7 Call Home session pump needs sibling + build/libnetconf.a)")
endif()
message(STATUS "  E7 SSH     EDGEHOST_E7_SSH_AVAILABLE=${EDGEHOST_E7_SSH_AVAILABLE}")
message(STATUS "  E7 chssh   EDGEHOST_E7_CHSSH_AVAILABLE=${EDGEHOST_E7_CHSSH_AVAILABLE}")
message(STATUS "  libyang    EDGEHOST_HAVE_LIBYANG=${EDGEHOST_HAVE_LIBYANG}")
message(STATUS "  liburing   ${LIBURING_LIBRARY}")

if(EDGEHOST_REQUIRE_SIBLINGS AND Shaggy_FOUND AND NOT SHAGGY_LIBRARY)
  message(FATAL_ERROR
    "shaggy headers found but no static lib (expected "
    "${SHAGGY_ROOT}/build/libhttp1.a). Build: cmake --build ${SHAGGY_ROOT}/build --target http1")
endif()
if(EDGEHOST_REQUIRE_LIBSIM AND Libsim_FOUND AND NOT LIBSIM_LIBRARY)
  message(FATAL_ERROR
    "libsim headers found but no build/libsim.a. Build: "
    "cmake -B ${LIBSIM_ROOT}/build -S ${LIBSIM_ROOT} && cmake --build ${LIBSIM_ROOT}/build")
endif()

# --- libedgecore ---
add_library(edgecore STATIC
  src/core/edgecore.c
  src/core/edge_config.c
  src/core/state_store.c
  src/core/auth_rbac.c
  src/core/pending_table.c
)
target_include_directories(edgecore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# --- Host: OpenSSL TLS helpers (server + client) ---
add_library(edgehost_tls STATIC src/host/tls_server.c)
target_include_directories(edgehost_tls PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(edgehost_tls PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# --- Host: plugin registry + outbound + WS hub + state notify choke point ---
add_library(edgehost_plugins STATIC
  src/host/plugin_host.c
  src/host/outbound_http.c
  src/host/ws_stream.c
  src/host/state_notify.c
  src/plugins/openai_proxy.c
  src/plugins/slack_plugin.c
  src/plugins/teams_plugin.c
)
target_include_directories(edgehost_plugins PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(edgehost_plugins PUBLIC edgecore edgehost_tls)

# --- Host: alloc ---
add_library(edgehost_alloc STATIC src/host/host_alloc.c)
target_include_directories(edgehost_alloc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# --- Host: E7 event apply + Call Home listen/pump (PR-3 / PR-4a) ---
add_library(edgehost_e7 STATIC
  src/host/e7_event_apply.c
  src/host/e7_callhome.c
)
target_include_directories(edgehost_e7 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
# state_notify + ws hub live in edgehost_plugins (put_and_notify / K16)
target_link_libraries(edgehost_e7 PUBLIC edgecore edgehost_plugins OpenSSL::Crypto)
if(Libnetconf_FOUND)
  target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_HAVE_LIBNETCONF=1)
  if(TARGET Libnetconf::netconf)
    target_link_libraries(edgehost_e7 PUBLIC Libnetconf::netconf)
  elseif(LIBNETCONF_LIBRARY)
    target_link_libraries(edgehost_e7 PUBLIC ${LIBNETCONF_LIBRARY})
  endif()
  if(TARGET Libnetconf::headers)
    target_link_libraries(edgehost_e7 PUBLIC Libnetconf::headers)
  endif()
  if(EDGEHOST_E7_SSH_AVAILABLE)
    target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_E7_SSH_AVAILABLE=1)
  else()
    target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_E7_SSH_AVAILABLE=0)
  endif()
  if(EDGEHOST_E7_CHSSH_AVAILABLE)
    target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_E7_CHSSH_AVAILABLE=1)
    if(TARGET Libchssh::chssh)
      target_link_libraries(edgehost_e7 PUBLIC Libchssh::chssh)
    elseif(LIBCHSSH_LIBRARY)
      target_link_libraries(edgehost_e7 PUBLIC ${LIBCHSSH_LIBRARY})
    endif()
  else()
    target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_E7_CHSSH_AVAILABLE=0)
  endif()
  # Legacy libassh symbols only when linking libnetconf SSH without chssh.
  if(EDGEHOST_E7_SSH_AVAILABLE AND NOT EDGEHOST_E7_CHSSH_AVAILABLE)
    target_compile_definitions(edgehost_e7 PUBLIC HAVE_LIBASSH=1)
  endif()
  if(EDGEHOST_HAVE_LIBYANG)
    target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_HAVE_LIBYANG=1)
    if(TARGET Libyang::yang)
      target_link_libraries(edgehost_e7 PUBLIC Libyang::yang)
    elseif(LIBYANG_LIBRARY)
      target_link_libraries(edgehost_e7 PUBLIC ${LIBYANG_LIBRARY})
    endif()
  else()
    target_compile_definitions(edgehost_e7 PUBLIC EDGEHOST_HAVE_LIBYANG=0)
  endif()
else()
  target_compile_definitions(edgehost_e7 PUBLIC
    EDGEHOST_HAVE_LIBNETCONF=0
    EDGEHOST_E7_SSH_AVAILABLE=0
    EDGEHOST_E7_CHSSH_AVAILABLE=0
    EDGEHOST_HAVE_LIBYANG=0)
endif()


# --- Host: YAML config + auth secret resolve ---
add_library(edgehost_config STATIC
  src/host/config_yaml.c
  src/host/config_hup.c
  src/host/auth_host.c
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

# --- Host: HTTP/1 serve + metrics + static + sidecar/notify ---
# (WS hub + state_notify live in edgehost_plugins so plugin state_put can fan-out)
add_library(edgehost_http STATIC
  src/host/edge_metrics.c
  src/host/edge_http1_serve.c
  src/host/static_files.c
  src/host/pq_sidecar.c
  src/host/notify_apply.c
)
target_include_directories(edgehost_http PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
# edgehost_e7: /api/v1/e7/* REST dispatch (PR-5)
target_link_libraries(edgehost_http PUBLIC edgecore edgehost_plugins edgehost_e7)
if(TARGET Shaggy::shaggy)
  target_link_libraries(edgehost_http PUBLIC Shaggy::shaggy)
elseif(SHAGGY_LIBRARY)
  target_link_libraries(edgehost_http PUBLIC ${SHAGGY_LIBRARY})
  if(SHAGGY_WEBSOCKET_LIBRARY)
    target_link_libraries(edgehost_http PUBLIC ${SHAGGY_WEBSOCKET_LIBRARY})
  endif()
endif()
if(Shaggy_FOUND AND TARGET Shaggy::headers)
  target_link_libraries(edgehost_http PUBLIC Shaggy::headers)
endif()

# --- Host: real io_uring loop + OpenSSL TLS server ---
add_library(edgehost_iouring STATIC src/host/iouring_loop.c)
target_include_directories(edgehost_iouring PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(edgehost_iouring PRIVATE ${LIBURING_INCLUDE_DIR})
target_link_libraries(edgehost_iouring PUBLIC
  edgecore edgehost_config edgehost_http edgehost_tls edgehost_e7
  ${LIBURING_LIBRARY})

# --- Host: class-A sim_main ---
if(Libsim_FOUND AND LIBSIM_LIBRARY)
  add_library(edgehost_sim STATIC src/host/sim_main.c)
  target_include_directories(edgehost_sim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(edgehost_sim PUBLIC
    edgecore edgehost_http edgehost_alloc
  )
  if(TARGET Libsim::sim)
    target_link_libraries(edgehost_sim PUBLIC Libsim::sim)
  else()
    target_link_libraries(edgehost_sim PUBLIC ${LIBSIM_LIBRARY})
  endif()
  if(TARGET Libsim::headers)
    target_link_libraries(edgehost_sim PUBLIC Libsim::headers)
  endif()
  set(EDGEHOST_HAVE_SIM 1)
else()
  set(EDGEHOST_HAVE_SIM 0)
endif()

# edgehost binary
add_executable(edgehost src/host/main.c)
target_link_libraries(edgehost PRIVATE
  edgecore edgehost_config edgehost_iouring edgehost_plugins edgehost_e7)

# Sibling header smoke
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
if(Libnetconf_FOUND AND TARGET Libnetconf::headers)
  target_link_libraries(edgehost_deps INTERFACE Libnetconf::headers)
endif()
# Optional libnetconf link for later e7_callhome (no host sources link yet).
if(Libnetconf_FOUND)
  add_library(edgehost_libnetconf INTERFACE)
  if(TARGET Libnetconf::netconf)
    target_link_libraries(edgehost_libnetconf INTERFACE Libnetconf::netconf)
  elseif(TARGET Libnetconf::headers)
    target_link_libraries(edgehost_libnetconf INTERFACE Libnetconf::headers)
  endif()
endif()

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
if(Libnetconf_FOUND)
  target_compile_definitions(edgehost_deps_smoke PRIVATE EDGEHOST_HAVE_LIBNETCONF=1)
endif()

add_executable(edgecore_smoke tests/test_edgecore_smoke.c)
target_link_libraries(edgecore_smoke PRIVATE edgecore)

add_executable(edgecore_alloc_test tests/test_edgecore_alloc.c)
target_link_libraries(edgecore_alloc_test PRIVATE edgecore edgehost_alloc)

add_executable(edgecore_config_test tests/test_edgecore_config.c)
target_link_libraries(edgecore_config_test PRIVATE edgecore edgehost_config)

add_executable(edgehost_iouring_test tests/test_iouring_static.c)
target_link_libraries(edgehost_iouring_test PRIVATE edgehost_iouring)

add_executable(edgehost_static_test tests/test_static_files.c)
target_link_libraries(edgehost_static_test PRIVATE edgehost_http edgehost_config)

add_executable(edgehost_state_test tests/test_state_store.c)
target_link_libraries(edgehost_state_test PRIVATE edgecore edgehost_http)

add_executable(edgehost_ws_test tests/test_ws_stream.c)
target_link_libraries(edgehost_ws_test PRIVATE edgecore edgehost_http)

add_executable(edgehost_auth_test tests/test_auth.c)
target_link_libraries(edgehost_auth_test PRIVATE edgecore edgehost_http edgehost_config)

add_executable(edgehost_pending_test tests/test_pending_plugin.c)
target_link_libraries(edgehost_pending_test PRIVATE edgecore edgehost_plugins)

add_executable(edgehost_openai_test tests/test_openai_proxy.c)
target_link_libraries(edgehost_openai_test PRIVATE
  edgecore edgehost_plugins edgehost_http pthread)

add_executable(edgehost_stubs_test tests/test_plugin_stubs.c)
target_link_libraries(edgehost_stubs_test PRIVATE edgecore edgehost_plugins edgehost_config)

add_executable(edgehost_tls_test tests/test_tls_server.c)
target_link_libraries(edgehost_tls_test PRIVATE edgehost_iouring)

add_executable(edgehost_sidecar_test tests/test_sidecar_notify.c)
target_link_libraries(edgehost_sidecar_test PRIVATE
  edgecore edgehost_plugins edgehost_http pthread)

add_executable(edgehost_e7_event_apply_test tests/test_e7_event_apply.c)
target_link_libraries(edgehost_e7_event_apply_test PRIVATE edgehost_e7)

add_executable(edgehost_junos_identity_test tests/test_junos_identity.c)
target_link_libraries(edgehost_junos_identity_test PRIVATE edgehost_e7 OpenSSL::Crypto)

add_executable(edgehost_e7_callhome_test tests/test_e7_callhome.c)
target_link_libraries(edgehost_e7_callhome_test PRIVATE edgehost_e7 pthread)
if(Libnetconf_FOUND)
  # Dialectic peer uses CALLHOME_SERVER role from libnetconf
  if(TARGET Libnetconf::netconf)
    target_link_libraries(edgehost_e7_callhome_test PRIVATE Libnetconf::netconf)
  elseif(LIBNETCONF_LIBRARY)
    target_link_libraries(edgehost_e7_callhome_test PRIVATE ${LIBNETCONF_LIBRARY})
  endif()
  if(TARGET Libnetconf::headers)
    target_link_libraries(edgehost_e7_callhome_test PRIVATE Libnetconf::headers)
  endif()
endif()

enable_testing()
add_test(NAME edgehost_deps_smoke COMMAND edgehost_deps_smoke)
add_test(NAME edgecore_smoke COMMAND edgecore_smoke)
add_test(NAME edgecore_alloc COMMAND edgecore_alloc_test)
add_test(NAME edgecore_config COMMAND edgecore_config_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME edgehost_iouring_static COMMAND edgehost_iouring_test)
add_test(NAME edgehost_static_files COMMAND edgehost_static_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME edgehost_state_store COMMAND edgehost_state_test)
add_test(NAME edgehost_ws_stream COMMAND edgehost_ws_test)
add_test(NAME edgehost_auth COMMAND edgehost_auth_test)
add_test(NAME edgehost_pending_plugin COMMAND edgehost_pending_test)
add_test(NAME edgehost_openai_proxy COMMAND edgehost_openai_test)
add_test(NAME edgehost_plugin_stubs COMMAND edgehost_stubs_test)
add_test(NAME edgehost_tls_server COMMAND edgehost_tls_test)
add_test(NAME edgehost_sidecar_notify COMMAND edgehost_sidecar_test)
add_test(NAME edgehost_e7_event_apply COMMAND edgehost_e7_event_apply_test)
add_test(NAME edgehost_junos_identity COMMAND edgehost_junos_identity_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME edgehost_e7_callhome COMMAND edgehost_e7_callhome_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})


if(EDGEHOST_HAVE_SIM)
  add_executable(edgehost_sim_test tests/test_sim_main.c)
  target_link_libraries(edgehost_sim_test PRIVATE edgehost_sim)
  add_test(NAME edgehost_sim_main COMMAND edgehost_sim_test)
endif()

if(BUILD_FUZZ AND EDGEHOST_HAVE_SIM)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(WARNING "BUILD_FUZZ is intended for Clang libFuzzer")
  endif()
  add_executable(fuzz_edgehost_a fuzz/fuzz_edgehost_a.c)
  target_link_libraries(fuzz_edgehost_a PRIVATE edgehost_sim)
  target_compile_options(fuzz_edgehost_a PRIVATE -fsanitize=fuzzer,address)
  target_link_options(fuzz_edgehost_a PRIVATE -fsanitize=fuzzer,address)
endif()

find_program(EDGEHOST_BASH bash)
if(EDGEHOST_BASH)
  add_test(
    NAME edgehost_verify_pins
    COMMAND ${EDGEHOST_BASH} "${CMAKE_CURRENT_SOURCE_DIR}/deps/verify_pins.sh" --allow-missing
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()

include(GNUInstallDirs)
set(_install_libs edgecore edgehost_alloc edgehost_config edgehost_tls
  edgehost_http edgehost_plugins edgehost_iouring edgehost_e7)
if(EDGEHOST_HAVE_SIM)
  list(APPEND _install_libs edgehost_sim)
endif()
install(TARGETS ${_install_libs} edgehost
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
install(DIRECTORY cmake/
  DESTINATION lib/cmake/edgehost
  FILES_MATCHING PATTERN "Find*.cmake" PATTERN "Edge*.cmake"
)
install(FILES deps/pins.txt DESTINATION share/edgehost)
install(FILES
  config/edgehost.example.yaml
  config/edgehost.lab.yaml
  config/edgehost.e7-lab.yaml
  DESTINATION share/edgehost/config)
