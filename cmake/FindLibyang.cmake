# FindLibyang.cmake — YANG schema plumbing (sibling libyang).
# Cache: LIBYANG_ROOT
#
# When found, sets EDGEHOST_HAVE_LIBYANG=1 and exports Libyang::yang.

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_yang_root "libyang" LIBYANG_ROOT)
set(LIBYANG_ROOT "${_yang_root}" CACHE PATH "Path to libyang checkout")

edgehost_find_header_tree(LIBYANG_INCLUDE_DIRS "${LIBYANG_ROOT}" "yang.h")
edgehost_try_import_static(LIBYANG_LIBRARY "${LIBYANG_ROOT}" "yang"
  "${LIBYANG_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libyang
  REQUIRED_VARS LIBYANG_ROOT LIBYANG_INCLUDE_DIRS LIBYANG_LIBRARY
  FAIL_MESSAGE "libyang not found. Build sibling: cmake -B ~/libyang/build -S ~/libyang && cmake --build ~/libyang/build"
)

set(EDGEHOST_HAVE_LIBYANG 0)
if(Libyang_FOUND AND LIBYANG_LIBRARY)
  set(EDGEHOST_HAVE_LIBYANG 1)
endif()

if(Libyang_FOUND)
  if(NOT TARGET Libyang::yang AND LIBYANG_LIBRARY)
    add_library(Libyang::yang ALIAS ${LIBYANG_LIBRARY})
  endif()
  message(STATUS "  libyang    FOUND LIB=${LIBYANG_LIBRARY} EDGEHOST_HAVE_LIBYANG=1")
else()
  message(STATUS "  libyang    FOUND=0 (optional; YANG path-form events need sibling build)")
endif()

mark_as_advanced(LIBYANG_ROOT LIBYANG_INCLUDE_DIRS)
