# FindLibnetconf.cmake — pure-C libnetconf checkout (E7 Call Home).
# Cache: LIBNETCONF_ROOT
#
# When found, exports Libnetconf::netconf / Libnetconf::headers.
# Static libnetconf built with HAVE_LIBASSH needs libassh (+ crypt crypto z)
# at link time; we attach those as INTERFACE deps when libassh is present.
# EDGEHOST_E7_SSH_AVAILABLE is set when both libnetconf and libassh are found.

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

# Optional libassh (SSH Call Home transport for static libnetconf).
find_path(ASSH_INCLUDE_DIR NAMES assh/assh.h
  PATHS /usr/local/include /usr/include
  DOC "Directory containing assh/assh.h")
find_library(ASSH_LIBRARY NAMES assh
  PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
  DOC "libassh shared or static library")

set(EDGEHOST_LIBASSH_FOUND 0)
if(ASSH_INCLUDE_DIR AND ASSH_LIBRARY)
  set(EDGEHOST_LIBASSH_FOUND 1)
endif()

# SSH Call Home is available if libchssh is present (preferred) or legacy
# libnetconf+libassh. EDGEHOST_E7_CHSSH_AVAILABLE is set by FindLibchssh.
if(NOT DEFINED EDGEHOST_E7_CHSSH_AVAILABLE)
  set(EDGEHOST_E7_CHSSH_AVAILABLE 0)
endif()
set(EDGEHOST_E7_SSH_AVAILABLE 0)
if(EDGEHOST_E7_CHSSH_AVAILABLE)
  set(EDGEHOST_E7_SSH_AVAILABLE 1)
elseif(Libnetconf_FOUND AND EDGEHOST_LIBASSH_FOUND AND LIBNETCONF_LIBRARY)
  set(EDGEHOST_E7_SSH_AVAILABLE 1)
endif()

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

  # Static netconf may reference libassh symbols; pull system deps when present.
  if(EDGEHOST_LIBASSH_FOUND AND LIBNETCONF_LIBRARY)
    # Resolve transitive libs used by libassh (from ldd of libassh.so).
    find_library(EDGEHOST_CRYPT_LIBRARY NAMES crypt)
    find_library(EDGEHOST_CRYPTO_LIBRARY NAMES crypto)
    find_library(EDGEHOST_Z_LIBRARY NAMES z)
    set(_assh_link "${ASSH_LIBRARY}")
    if(EDGEHOST_CRYPT_LIBRARY)
      list(APPEND _assh_link "${EDGEHOST_CRYPT_LIBRARY}")
    else()
      list(APPEND _assh_link crypt)
    endif()
    if(EDGEHOST_CRYPTO_LIBRARY)
      list(APPEND _assh_link "${EDGEHOST_CRYPTO_LIBRARY}")
    else()
      list(APPEND _assh_link crypto)
    endif()
    if(EDGEHOST_Z_LIBRARY)
      list(APPEND _assh_link "${EDGEHOST_Z_LIBRARY}")
    else()
      list(APPEND _assh_link z)
    endif()
    set_property(TARGET ${LIBNETCONF_LIBRARY} APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES ${_assh_link})
    set_property(TARGET ${LIBNETCONF_LIBRARY} APPEND PROPERTY
      INTERFACE_INCLUDE_DIRECTORIES "${ASSH_INCLUDE_DIR}")
    message(STATUS "  libassh    FOUND ASSH_LIBRARY=${ASSH_LIBRARY}")
  else()
    message(STATUS "  libassh    FOUND=0 (E7 SSH Call Home disabled)")
  endif()
endif()

if(EDGEHOST_E7_SSH_AVAILABLE)
  if(EDGEHOST_E7_CHSSH_AVAILABLE)
    message(STATUS "  E7 SSH     EDGEHOST_E7_SSH_AVAILABLE=1 (libchssh preferred)")
  else()
    message(STATUS "  E7 SSH     EDGEHOST_E7_SSH_AVAILABLE=1 (libnetconf + libassh legacy)")
  endif()
else()
  message(STATUS "  E7 SSH     EDGEHOST_E7_SSH_AVAILABLE=0")
endif()

set(ASSH_INCLUDE_DIR "${ASSH_INCLUDE_DIR}" CACHE PATH "libassh include parent")
set(ASSH_LIBRARY "${ASSH_LIBRARY}" CACHE FILEPATH "libassh library")

mark_as_advanced(LIBNETCONF_ROOT LIBNETCONF_INCLUDE_DIRS ASSH_INCLUDE_DIR ASSH_LIBRARY)
