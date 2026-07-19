# FindShaggy.cmake — locate pure-C shaggy (HTTP/1+2+WebSocket) checkout.
# Usage: find_package(Shaggy REQUIRED)  or  find_package(Shaggy)
# Cache: SHAGGY_ROOT
# Provides: Shaggy::headers, Shaggy::shaggy (http1 + websocket when built)

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_shaggy_root "shaggy" SHAGGY_ROOT)
set(SHAGGY_ROOT "${_shaggy_root}" CACHE PATH "Path to shaggy checkout")

edgehost_find_header_tree(SHAGGY_INCLUDE_DIRS "${SHAGGY_ROOT}" "http1.h")
# Prefer libhttp1.a (current shaggy CMake); fall back to legacy names.
edgehost_try_import_static(SHAGGY_HTTP1_LIBRARY "${SHAGGY_ROOT}" "http1" "${SHAGGY_INCLUDE_DIRS}")
edgehost_try_import_static(SHAGGY_WEBSOCKET_LIBRARY "${SHAGGY_ROOT}" "websocket" "${SHAGGY_INCLUDE_DIRS}")
if(NOT SHAGGY_HTTP1_LIBRARY)
  edgehost_try_import_static(SHAGGY_HTTP1_LIBRARY "${SHAGGY_ROOT}" "http" "${SHAGGY_INCLUDE_DIRS}")
endif()
if(NOT SHAGGY_HTTP1_LIBRARY)
  edgehost_try_import_static(SHAGGY_HTTP1_LIBRARY "${SHAGGY_ROOT}" "shaggy" "${SHAGGY_INCLUDE_DIRS}")
endif()
set(SHAGGY_LIBRARY "${SHAGGY_HTTP1_LIBRARY}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Shaggy
  REQUIRED_VARS SHAGGY_ROOT SHAGGY_INCLUDE_DIRS
  FAIL_MESSAGE "shaggy not found. Set -DSHAGGY_ROOT=… (expected include/http1.h)"
)

if(Shaggy_FOUND)
  set(SHAGGY_INCLUDE_DIRS "${SHAGGY_INCLUDE_DIRS}")
  if(SHAGGY_HTTP1_LIBRARY OR SHAGGY_WEBSOCKET_LIBRARY)
    set(SHAGGY_LIBRARIES ${SHAGGY_HTTP1_LIBRARY} ${SHAGGY_WEBSOCKET_LIBRARY})
  endif()
  if(NOT TARGET Shaggy::shaggy)
    if(SHAGGY_HTTP1_LIBRARY OR SHAGGY_WEBSOCKET_LIBRARY)
      add_library(Shaggy::shaggy INTERFACE IMPORTED)
      set(_shaggy_link "")
      if(SHAGGY_HTTP1_LIBRARY)
        list(APPEND _shaggy_link ${SHAGGY_HTTP1_LIBRARY})
      endif()
      if(SHAGGY_WEBSOCKET_LIBRARY)
        list(APPEND _shaggy_link ${SHAGGY_WEBSOCKET_LIBRARY})
      endif()
      set_target_properties(Shaggy::shaggy PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_shaggy_link}"
        INTERFACE_INCLUDE_DIRECTORIES "${SHAGGY_INCLUDE_DIRS}"
      )
    endif()
  endif()
  if(NOT TARGET Shaggy::headers)
    add_library(Shaggy::headers INTERFACE IMPORTED)
    set_target_properties(Shaggy::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${SHAGGY_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(SHAGGY_ROOT SHAGGY_INCLUDE_DIRS)
