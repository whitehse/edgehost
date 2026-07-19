# FindShaggy.cmake — locate pure-C shaggy (HTTP/1+2+WebSocket) checkout.
# Usage: find_package(Shaggy REQUIRED)  or  find_package(Shaggy)
# Cache: SHAGGY_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_shaggy_root "shaggy" SHAGGY_ROOT)
set(SHAGGY_ROOT "${_shaggy_root}" CACHE PATH "Path to shaggy checkout")

edgehost_find_header_tree(SHAGGY_INCLUDE_DIRS "${SHAGGY_ROOT}" "http1.h")
edgehost_try_import_static(SHAGGY_LIBRARY "${SHAGGY_ROOT}" "http" "${SHAGGY_INCLUDE_DIRS}")
# shaggy library target name may be libhttp / libshaggy depending on tree
if(NOT SHAGGY_LIBRARY)
  edgehost_try_import_static(SHAGGY_LIBRARY "${SHAGGY_ROOT}" "shaggy" "${SHAGGY_INCLUDE_DIRS}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Shaggy
  REQUIRED_VARS SHAGGY_ROOT SHAGGY_INCLUDE_DIRS
  FAIL_MESSAGE "shaggy not found. Set -DSHAGGY_ROOT=… (expected include/http1.h)"
)

if(Shaggy_FOUND)
  set(SHAGGY_INCLUDE_DIRS "${SHAGGY_INCLUDE_DIRS}")
  if(SHAGGY_LIBRARY)
    set(SHAGGY_LIBRARIES ${SHAGGY_LIBRARY})
  endif()
  if(NOT TARGET Shaggy::shaggy AND SHAGGY_LIBRARY)
    add_library(Shaggy::shaggy ALIAS ${SHAGGY_LIBRARY})
  endif()
  if(NOT TARGET Shaggy::headers)
    add_library(Shaggy::headers INTERFACE IMPORTED)
    set_target_properties(Shaggy::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${SHAGGY_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(SHAGGY_ROOT SHAGGY_INCLUDE_DIRS)
