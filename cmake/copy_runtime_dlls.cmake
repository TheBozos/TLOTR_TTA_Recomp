# Copies the DLLs an executable needs from the toolchain's bin folder (MSYS2 UCRT64) next to it, so it starts from
# Explorer without MSYS2 on the PATH. Run after each link (CMakeLists.txt, MinGW only):
#   cmake -DEXE=<exe> -DSEARCH=<toolchain bin> -P cmake/copy_runtime_dlls.cmake
# Windows' own DLLs (system32, the api-ms-win sets) are left out. Only copied when missing or changed.
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW) # paths normalized before the exclusions match
endif()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${EXE}"
  DIRECTORIES "${SEARCH}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
  POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32" "[Ss][Yy][Ss][Ww][Oo][Ww]64")
get_filename_component(dir "${EXE}" DIRECTORY)
set(copied 0)
foreach(dll IN LISTS resolved)
  get_filename_component(name "${dll}" NAME)
  # (IS_NEWER_THAN is true for equal times too: file(COPY) keeps the time, so an unchanged DLL is skipped)
  if(NOT EXISTS "${dir}/${name}" OR NOT "${dir}/${name}" IS_NEWER_THAN "${dll}")
    file(COPY "${dll}" DESTINATION "${dir}")
    math(EXPR copied "${copied} + 1")
  endif()
endforeach()
list(LENGTH resolved total)
if(copied GREATER 0)
  message(STATUS "copy_runtime_dlls: ${copied} of ${total} DLLs copied next to ${EXE}")
endif()
foreach(name IN LISTS unresolved)
  if(NOT name MATCHES "^(api-ms-|ext-ms-)")
    message(WARNING "copy_runtime_dlls: ${name} not found in ${SEARCH}")
  endif()
endforeach()
