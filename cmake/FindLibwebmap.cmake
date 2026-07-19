# FindLibwebmap.cmake — libwebmap (SPA package consumer; not linked into core yet).
# Cache: LIBWEBMAP_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_webmap_root "libwebmap" LIBWEBMAP_ROOT)
set(LIBWEBMAP_ROOT "${_webmap_root}" CACHE PATH "Path to libwebmap checkout")

edgehost_find_header_tree(LIBWEBMAP_INCLUDE_DIRS "${LIBWEBMAP_ROOT}" "webmap.h")
edgehost_try_import_static(LIBWEBMAP_LIBRARY "${LIBWEBMAP_ROOT}" "webmap" "${LIBWEBMAP_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libwebmap
  REQUIRED_VARS LIBWEBMAP_ROOT LIBWEBMAP_INCLUDE_DIRS
  FAIL_MESSAGE "libwebmap not found. Set -DLIBWEBMAP_ROOT=… (expected include/webmap.h)"
)

if(Libwebmap_FOUND)
  if(LIBWEBMAP_LIBRARY)
    set(LIBWEBMAP_LIBRARIES ${LIBWEBMAP_LIBRARY})
  endif()
  if(NOT TARGET Libwebmap::webmap AND LIBWEBMAP_LIBRARY)
    add_library(Libwebmap::webmap ALIAS ${LIBWEBMAP_LIBRARY})
  endif()
  if(NOT TARGET Libwebmap::headers)
    add_library(Libwebmap::headers INTERFACE IMPORTED)
    set_target_properties(Libwebmap::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBWEBMAP_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBWEBMAP_ROOT LIBWEBMAP_INCLUDE_DIRS)
