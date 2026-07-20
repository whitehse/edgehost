# FindLibchssh.cmake — Call Home SSH transport (sibling libchssh).
# Cache: LIBCHSSH_ROOT
#
# When found, sets EDGEHOST_E7_CHSSH_AVAILABLE=1 and exports Libchssh::chssh.

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_chssh_root "libchssh" LIBCHSSH_ROOT)
set(LIBCHSSH_ROOT "${_chssh_root}" CACHE PATH "Path to libchssh checkout")

edgehost_find_header_tree(LIBCHSSH_INCLUDE_DIRS "${LIBCHSSH_ROOT}" "chssh.h")
edgehost_try_import_static(LIBCHSSH_LIBRARY "${LIBCHSSH_ROOT}" "chssh"
  "${LIBCHSSH_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libchssh
  REQUIRED_VARS LIBCHSSH_ROOT LIBCHSSH_INCLUDE_DIRS LIBCHSSH_LIBRARY
  FAIL_MESSAGE "libchssh not found. Build sibling: cmake -B ~/libchssh/build -S ~/libchssh && cmake --build ~/libchssh/build"
)

set(EDGEHOST_E7_CHSSH_AVAILABLE 0)
if(Libchssh_FOUND AND LIBCHSSH_LIBRARY)
  set(EDGEHOST_E7_CHSSH_AVAILABLE 1)
endif()

if(Libchssh_FOUND)
  if(NOT TARGET Libchssh::chssh AND LIBCHSSH_LIBRARY)
    add_library(Libchssh::chssh ALIAS ${LIBCHSSH_LIBRARY})
  endif()
  # Production crypto needs OpenSSL (already required by edgehost).
  if(TARGET ${LIBCHSSH_LIBRARY})
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
      set_property(TARGET ${LIBCHSSH_LIBRARY} APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    endif()
  endif()
  message(STATUS "  libchssh   FOUND LIB=${LIBCHSSH_LIBRARY} EDGEHOST_E7_CHSSH_AVAILABLE=1")
else()
  message(STATUS "  libchssh   FOUND=0 (prefer build ~/libchssh for E7 SSH Call Home)")
endif()

mark_as_advanced(LIBCHSSH_ROOT LIBCHSSH_INCLUDE_DIRS)
