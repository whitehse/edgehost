# FindLibsim.cmake — shared sim stack (Track 0). Optional until P0.1 exists.
# Cache: LIBSIM_ROOT

include("${CMAKE_CURRENT_LIST_DIR}/EdgeDepsCommon.cmake")

edgehost_resolve_sibling_root(_sim_root "libsim" LIBSIM_ROOT)
set(LIBSIM_ROOT "${_sim_root}" CACHE PATH "Path to libsim checkout (optional until P0.1)")

set(_sim_inc "")
if(EXISTS "${LIBSIM_ROOT}/include/sim.h")
  set(_sim_inc "${LIBSIM_ROOT}/include")
elseif(EXISTS "${LIBSIM_ROOT}/include/libsim.h")
  set(_sim_inc "${LIBSIM_ROOT}/include")
endif()
set(LIBSIM_INCLUDE_DIRS "${_sim_inc}")

include(FindPackageHandleStandardArgs)
# Soft: do not REQUIRED by default; package is "found" only if headers exist.
find_package_handle_standard_args(Libsim
  REQUIRED_VARS LIBSIM_ROOT LIBSIM_INCLUDE_DIRS
  FAIL_MESSAGE "libsim not found (expected after P0.1). Set -DLIBSIM_ROOT=… when available."
)

if(Libsim_FOUND AND NOT TARGET Libsim::headers)
  add_library(Libsim::headers INTERFACE IMPORTED)
  set_target_properties(Libsim::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIBSIM_INCLUDE_DIRS}"
  )
endif()

mark_as_advanced(LIBSIM_ROOT LIBSIM_INCLUDE_DIRS)
