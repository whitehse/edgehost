# FindLibyaml.cmake — pure-C libyaml checkout.
# Cache: LIBYAML_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_yaml_root "libyaml" LIBYAML_ROOT)
set(LIBYAML_ROOT "${_yaml_root}" CACHE PATH "Path to libyaml checkout")

edgehost_find_header_tree(LIBYAML_INCLUDE_DIRS "${LIBYAML_ROOT}" "yaml.h")
edgehost_try_import_static(LIBYAML_LIBRARY "${LIBYAML_ROOT}" "yaml" "${LIBYAML_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libyaml
  REQUIRED_VARS LIBYAML_ROOT LIBYAML_INCLUDE_DIRS
  FAIL_MESSAGE "libyaml not found. Set -DLIBYAML_ROOT=… (expected include/yaml.h)"
)

if(Libyaml_FOUND)
  if(LIBYAML_LIBRARY)
    set(LIBYAML_LIBRARIES ${LIBYAML_LIBRARY})
  endif()
  if(NOT TARGET Libyaml::yaml AND LIBYAML_LIBRARY)
    add_library(Libyaml::yaml ALIAS ${LIBYAML_LIBRARY})
  endif()
  if(NOT TARGET Libyaml::headers)
    add_library(Libyaml::headers INTERFACE IMPORTED)
    set_target_properties(Libyaml::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBYAML_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBYAML_ROOT LIBYAML_INCLUDE_DIRS)
