# FindLibnetconf.cmake — pure-C libnetconf checkout (E7 Call Home).
# Cache: LIBNETCONF_ROOT
#
# Optional dependency for now: edgehost configures without it.
# When found, exports Libnetconf::netconf / Libnetconf::headers for later
# e7_callhome session pump (PR-4+). No listen code in PR-2.

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_netconf_root "libnetconf" LIBNETCONF_ROOT)
set(LIBNETCONF_ROOT "${_netconf_root}" CACHE PATH "Path to libnetconf checkout")

edgehost_find_header_tree(LIBNETCONF_INCLUDE_DIRS "${LIBNETCONF_ROOT}" "netconf.h")
edgehost_try_import_static(LIBNETCONF_LIBRARY "${LIBNETCONF_ROOT}" "netconf"
  "${LIBNETCONF_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libnetconf
  REQUIRED_VARS LIBNETCONF_ROOT LIBNETCONF_INCLUDE_DIRS
  FAIL_MESSAGE "libnetconf not found. Set -DLIBNETCONF_ROOT=… (expected include/netconf.h)"
)

if(Libnetconf_FOUND)
  if(LIBNETCONF_LIBRARY)
    set(LIBNETCONF_LIBRARIES ${LIBNETCONF_LIBRARY})
  endif()
  if(NOT TARGET Libnetconf::netconf AND LIBNETCONF_LIBRARY)
    add_library(Libnetconf::netconf ALIAS ${LIBNETCONF_LIBRARY})
  endif()
  if(NOT TARGET Libnetconf::headers)
    add_library(Libnetconf::headers INTERFACE IMPORTED)
    set_target_properties(Libnetconf::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBNETCONF_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBNETCONF_ROOT LIBNETCONF_INCLUDE_DIRS)
