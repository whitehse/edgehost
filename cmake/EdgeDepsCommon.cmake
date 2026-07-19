# Shared helpers for edgehost Find*.cmake modules (P1.0).
#
# Sibling layout convention (from program design / pqproxy):
#   -DSHAGGY_ROOT=$HOME/shaggy  (etc.)
#   or SIBLING_ROOT=$HOME and repo name under it
#   or default $HOME/<repo>
#
# Each Find module sets:
#   <Name>_FOUND, <Name>_ROOT, <Name>_INCLUDE_DIRS
# and optionally <Name>_LIBRARY / <Name>_LIBRARIES when a built artifact exists.

if(DEFINED EDGEHOST_DEPS_COMMON_INCLUDED)
  return()
endif()
set(EDGEHOST_DEPS_COMMON_INCLUDED TRUE)

# Parent of this file is cmake/; repo root is parent of that.
get_filename_component(EDGEHOST_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED SIBLING_ROOT OR SIBLING_ROOT STREQUAL "")
  if(DEFINED ENV{SIBLING_ROOT} AND NOT "$ENV{SIBLING_ROOT}" STREQUAL "")
    set(SIBLING_ROOT "$ENV{SIBLING_ROOT}")
  elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(SIBLING_ROOT "$ENV{HOME}")
  else()
    set(SIBLING_ROOT "/home/dwhite")
  endif()
endif()

# ---------------------------------------------------------------------------
# edgehost_resolve_sibling_root(<OUT_VAR> <REPO_NAME> <ROOT_CACHE_VAR>)
# Prefer explicit -DROOT_CACHE_VAR=…, else SIBLING_ROOT/repo, else $HOME/repo.
# ---------------------------------------------------------------------------
function(edgehost_resolve_sibling_root out_var repo_name root_cache_var)
  if(DEFINED ${root_cache_var} AND NOT "${${root_cache_var}}" STREQUAL "")
    set(_root "${${root_cache_var}}")
  elseif(EXISTS "${SIBLING_ROOT}/${repo_name}")
    set(_root "${SIBLING_ROOT}/${repo_name}")
  elseif(DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/${repo_name}")
    set(_root "$ENV{HOME}/${repo_name}")
  else()
    set(_root "${SIBLING_ROOT}/${repo_name}")
  endif()
  set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# edgehost_find_header_tree(<OUT_INC> <ROOT> <HEADER_REL>)
# Sets OUT_INC to ROOT/include if HEADER_REL exists under it.
# ---------------------------------------------------------------------------
function(edgehost_find_header_tree out_inc root header_rel)
  set(_inc "${root}/include")
  if(EXISTS "${_inc}/${header_rel}")
    set(${out_inc} "${_inc}" PARENT_SCOPE)
  else()
    set(${out_inc} "" PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# edgehost_try_import_static(<OUT_TARGET> <ROOT> <LIBNAME> <INC>)
# Prefer ROOT/build/libNAME.a if present; else leave OUT_TARGET empty
# (caller may add sources later).
# ---------------------------------------------------------------------------
function(edgehost_try_import_static out_target root libname inc)
  set(_lib "${root}/build/lib${libname}.a")
  if(EXISTS "${_lib}" AND NOT "${inc}" STREQUAL "")
    if(NOT TARGET edgehost_imp_${libname})
      add_library(edgehost_imp_${libname} STATIC IMPORTED GLOBAL)
      set_target_properties(edgehost_imp_${libname} PROPERTIES
        IMPORTED_LOCATION "${_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${inc}"
      )
    endif()
    set(${out_target} edgehost_imp_${libname} PARENT_SCOPE)
  else()
    set(${out_target} "" PARENT_SCOPE)
  endif()
endfunction()
