# FindLibharness.cmake — libharness AI session library.
# Cache: LIBHARNESS_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_harness_root "libharness" LIBHARNESS_ROOT)
set(LIBHARNESS_ROOT "${_harness_root}" CACHE PATH "Path to libharness checkout")

edgehost_find_header_tree(LIBHARNESS_INCLUDE_DIRS "${LIBHARNESS_ROOT}" "harness.h")
edgehost_try_import_static(LIBHARNESS_LIBRARY "${LIBHARNESS_ROOT}" "harness" "${LIBHARNESS_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libharness
  REQUIRED_VARS LIBHARNESS_ROOT LIBHARNESS_INCLUDE_DIRS
  FAIL_MESSAGE "libharness not found. Set -DLIBHARNESS_ROOT=… (expected include/harness.h)"
)

if(Libharness_FOUND)
  if(LIBHARNESS_LIBRARY)
    set(LIBHARNESS_LIBRARIES ${LIBHARNESS_LIBRARY})
  endif()
  if(NOT TARGET Libharness::harness AND LIBHARNESS_LIBRARY)
    add_library(Libharness::harness ALIAS ${LIBHARNESS_LIBRARY})
  endif()
  if(NOT TARGET Libharness::headers)
    add_library(Libharness::headers INTERFACE IMPORTED)
    set_target_properties(Libharness::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBHARNESS_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBHARNESS_ROOT LIBHARNESS_INCLUDE_DIRS)
