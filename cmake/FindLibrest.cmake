# FindLibrest.cmake — pure-C librest checkout.
# Cache: LIBREST_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_rest_root "librest" LIBREST_ROOT)
set(LIBREST_ROOT "${_rest_root}" CACHE PATH "Path to librest checkout")

edgehost_find_header_tree(LIBREST_INCLUDE_DIRS "${LIBREST_ROOT}" "rest.h")
edgehost_try_import_static(LIBREST_LIBRARY "${LIBREST_ROOT}" "rest" "${LIBREST_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Librest
  REQUIRED_VARS LIBREST_ROOT LIBREST_INCLUDE_DIRS
  FAIL_MESSAGE "librest not found. Set -DLIBREST_ROOT=… (expected include/rest.h)"
)

if(Librest_FOUND)
  if(LIBREST_LIBRARY)
    set(LIBREST_LIBRARIES ${LIBREST_LIBRARY})
  endif()
  if(NOT TARGET Librest::rest AND LIBREST_LIBRARY)
    add_library(Librest::rest ALIAS ${LIBREST_LIBRARY})
  endif()
  if(NOT TARGET Librest::headers)
    add_library(Librest::headers INTERFACE IMPORTED)
    set_target_properties(Librest::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBREST_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBREST_ROOT LIBREST_INCLUDE_DIRS)
