# FindLibanim.cmake — libanim scene engine (explain SPA / plan validation).
# Cache: LIBANIM_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_anim_root "libanim" LIBANIM_ROOT)
set(LIBANIM_ROOT "${_anim_root}" CACHE PATH "Path to libanim checkout")

edgehost_find_header_tree(LIBANIM_INCLUDE_DIRS "${LIBANIM_ROOT}" "anim.h")
edgehost_try_import_static(LIBANIM_LIBRARY "${LIBANIM_ROOT}" "anim" "${LIBANIM_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libanim
  REQUIRED_VARS LIBANIM_ROOT LIBANIM_INCLUDE_DIRS
  FAIL_MESSAGE "libanim not found. Set -DLIBANIM_ROOT=… (expected include/anim.h)"
)

if(Libanim_FOUND)
  if(LIBANIM_LIBRARY)
    set(LIBANIM_LIBRARIES ${LIBANIM_LIBRARY})
  endif()
  if(NOT TARGET Libanim::anim AND LIBANIM_LIBRARY)
    add_library(Libanim::anim ALIAS ${LIBANIM_LIBRARY})
  endif()
  if(NOT TARGET Libanim::headers)
    add_library(Libanim::headers INTERFACE IMPORTED)
    set_target_properties(Libanim::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBANIM_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBANIM_ROOT LIBANIM_INCLUDE_DIRS)
