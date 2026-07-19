# FindPique.cmake — pique (libpqwire) checkout.
# Cache: PIQUE_ROOT (matches pqproxy convention)

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_pique_root "pique" PIQUE_ROOT)
set(PIQUE_ROOT "${_pique_root}" CACHE PATH "Path to pique (libpqwire) checkout")

edgehost_find_header_tree(PIQUE_INCLUDE_DIRS "${PIQUE_ROOT}" "pqwire.h")
edgehost_try_import_static(PIQUE_LIBRARY "${PIQUE_ROOT}" "pqwire" "${PIQUE_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Pique
  REQUIRED_VARS PIQUE_ROOT PIQUE_INCLUDE_DIRS
  FAIL_MESSAGE "pique not found. Set -DPIQUE_ROOT=… (expected include/pqwire.h)"
)

if(Pique_FOUND)
  if(PIQUE_LIBRARY)
    set(PIQUE_LIBRARIES ${PIQUE_LIBRARY})
  endif()
  if(NOT TARGET Pique::pqwire AND PIQUE_LIBRARY)
    add_library(Pique::pqwire ALIAS ${PIQUE_LIBRARY})
  endif()
  if(NOT TARGET Pique::headers)
    add_library(Pique::headers INTERFACE IMPORTED)
    set_target_properties(Pique::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${PIQUE_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(PIQUE_ROOT PIQUE_INCLUDE_DIRS)
