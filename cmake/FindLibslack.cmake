# FindLibslack.cmake — libslack protocol library.
# Cache: LIBSLACK_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_slack_root "libslack" LIBSLACK_ROOT)
set(LIBSLACK_ROOT "${_slack_root}" CACHE PATH "Path to libslack checkout")

edgehost_find_header_tree(LIBSLACK_INCLUDE_DIRS "${LIBSLACK_ROOT}" "slack.h")
edgehost_try_import_static(LIBSLACK_LIBRARY "${LIBSLACK_ROOT}" "slack" "${LIBSLACK_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libslack
  REQUIRED_VARS LIBSLACK_ROOT LIBSLACK_INCLUDE_DIRS
  FAIL_MESSAGE "libslack not found. Set -DLIBSLACK_ROOT=… (expected include/slack.h)"
)

if(Libslack_FOUND)
  if(LIBSLACK_LIBRARY)
    set(LIBSLACK_LIBRARIES ${LIBSLACK_LIBRARY})
  endif()
  if(NOT TARGET Libslack::slack AND LIBSLACK_LIBRARY)
    add_library(Libslack::slack ALIAS ${LIBSLACK_LIBRARY})
  endif()
  if(NOT TARGET Libslack::headers)
    add_library(Libslack::headers INTERFACE IMPORTED)
    set_target_properties(Libslack::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBSLACK_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBSLACK_ROOT LIBSLACK_INCLUDE_DIRS)
