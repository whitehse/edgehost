# FindLibteams.cmake — libteams protocol library.
# Cache: LIBTEAMS_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_teams_root "libteams" LIBTEAMS_ROOT)
set(LIBTEAMS_ROOT "${_teams_root}" CACHE PATH "Path to libteams checkout")

edgehost_find_header_tree(LIBTEAMS_INCLUDE_DIRS "${LIBTEAMS_ROOT}" "teams.h")
edgehost_try_import_static(LIBTEAMS_LIBRARY "${LIBTEAMS_ROOT}" "teams" "${LIBTEAMS_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libteams
  REQUIRED_VARS LIBTEAMS_ROOT LIBTEAMS_INCLUDE_DIRS
  FAIL_MESSAGE "libteams not found. Set -DLIBTEAMS_ROOT=… (expected include/teams.h)"
)

if(Libteams_FOUND)
  if(LIBTEAMS_LIBRARY)
    set(LIBTEAMS_LIBRARIES ${LIBTEAMS_LIBRARY})
  endif()
  if(NOT TARGET Libteams::teams AND LIBTEAMS_LIBRARY)
    add_library(Libteams::teams ALIAS ${LIBTEAMS_LIBRARY})
  endif()
  if(NOT TARGET Libteams::headers)
    add_library(Libteams::headers INTERFACE IMPORTED)
    set_target_properties(Libteams::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBTEAMS_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBTEAMS_ROOT LIBTEAMS_INCLUDE_DIRS)
