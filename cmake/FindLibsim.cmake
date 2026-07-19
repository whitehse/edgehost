# FindLibsim.cmake — shared sim stack (Track 0). Required for P1.5 class-A fuzz.
# Cache: LIBSIM_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_sim_root "libsim" LIBSIM_ROOT)
set(LIBSIM_ROOT "${_sim_root}" CACHE PATH "Path to libsim checkout (Track 0)")

set(_sim_inc "")
if(EXISTS "${LIBSIM_ROOT}/include/sim.h")
  set(_sim_inc "${LIBSIM_ROOT}/include")
endif()
set(LIBSIM_INCLUDE_DIRS "${_sim_inc}")

edgehost_try_import_static(LIBSIM_LIBRARY "${LIBSIM_ROOT}" "sim" "${LIBSIM_INCLUDE_DIRS}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libsim
  REQUIRED_VARS LIBSIM_ROOT LIBSIM_INCLUDE_DIRS
  FAIL_MESSAGE "libsim not found. Set -DLIBSIM_ROOT=… (expected include/sim.h)"
)

if(Libsim_FOUND)
  if(LIBSIM_LIBRARY)
    set(LIBSIM_LIBRARIES ${LIBSIM_LIBRARY})
  endif()
  if(NOT TARGET Libsim::sim AND LIBSIM_LIBRARY)
    add_library(Libsim::sim ALIAS ${LIBSIM_LIBRARY})
  endif()
  if(NOT TARGET Libsim::headers)
    add_library(Libsim::headers INTERFACE IMPORTED)
    set_target_properties(Libsim::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBSIM_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(LIBSIM_ROOT LIBSIM_INCLUDE_DIRS)
