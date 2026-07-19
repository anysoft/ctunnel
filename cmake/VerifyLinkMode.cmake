if(NOT DEFINED BINARY OR NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "Link-mode verification binary does not exist: ${BINARY}")
endif()
if(NOT DEFINED REPORT)
  set(REPORT "${BINARY}.link-report.txt")
endif()

file(SIZE "${BINARY}" BINARY_SIZE)
file(WRITE "${REPORT}"
  "binary: ${BINARY}\n"
  "file_size_bytes: ${BINARY_SIZE}\n"
  "link_mode: ${MODE}\n"
  "libc: ${LIBC}\n"
  "compiler: ${COMPILER_ID} ${COMPILER_VERSION}\n"
  "compiler_path: ${COMPILER_PATH}\n"
  "target_triplet: ${TARGET_TRIPLET}\n"
  "target_system: ${TARGET_SYSTEM}\n")

function(run_tool output_variable label tool)
  if(NOT tool OR tool MATCHES "-NOTFOUND$")
    file(APPEND "${REPORT}" "\n${label}: unavailable\n")
    set(${output_variable} "" PARENT_SCOPE)
    return()
  endif()
  execute_process(
    COMMAND "${tool}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  file(APPEND "${REPORT}" "\n${label} (${tool}; exit ${result}):\n${output}${error}")
  set(${output_variable} "${output}${error}" PARENT_SCOPE)
  set(${output_variable}_RESULT "${result}" PARENT_SCOPE)
endfunction()

function(reject_crypto_dependencies dependency_text)
  string(TOLOWER "${dependency_text}" lower_dependencies)
  if(lower_dependencies MATCHES
      "libsodium|libssl|libcrypto|libmbedtls|libmonocypher|monocypher\\.(so|dylib|dll)")
    message(FATAL_ERROR
      "${MODE} ctunnel unexpectedly depends on a third-party crypto shared library.\n"
      "See ${REPORT}")
  endif()
endfunction()

run_tool(FILE_OUTPUT "file" "${FILE_TOOL}" "${BINARY}")
if(SIZE_TOOL AND NOT SIZE_TOOL MATCHES "-NOTFOUND$")
  if(TARGET_SYSTEM STREQUAL "Darwin")
    run_tool(SIZE_OUTPUT "sections (.text/.rodata/.data/.bss)" "${SIZE_TOOL}" -m "${BINARY}")
  else()
    run_tool(SIZE_OUTPUT "sections (.text/.rodata/.data/.bss)" "${SIZE_TOOL}" -A "${BINARY}")
  endif()
elseif(TARGET_SYSTEM STREQUAL "Windows" AND DUMPBIN_TOOL AND
       NOT DUMPBIN_TOOL MATCHES "-NOTFOUND$")
  run_tool(SIZE_OUTPUT "sections (.text/.rodata/.data/.bss)" "${DUMPBIN_TOOL}"
           /headers "${BINARY}")
elseif(TARGET_SYSTEM STREQUAL "Windows" AND OBJDUMP_TOOL AND
       NOT OBJDUMP_TOOL MATCHES "-NOTFOUND$")
  run_tool(SIZE_OUTPUT "sections (.text/.rodata/.data/.bss)" "${OBJDUMP_TOOL}"
           -h "${BINARY}")
else()
  file(APPEND "${REPORT}" "\nsections (.text/.rodata/.data/.bss): unavailable\n")
endif()

if(TARGET_SYSTEM STREQUAL "Linux")
  if(NOT READELF_TOOL OR READELF_TOOL MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
      "readelf is required to verify Linux link mode; install target binutils or set CMAKE_READELF")
  endif()
  run_tool(ELF_PROGRAM_HEADERS "readelf program headers" "${READELF_TOOL}" -W -l "${BINARY}")
  run_tool(ELF_DYNAMIC "readelf dynamic section" "${READELF_TOOL}" -W -d "${BINARY}")
  if(NOT ELF_PROGRAM_HEADERS_RESULT EQUAL 0 OR NOT ELF_DYNAMIC_RESULT EQUAL 0)
    message(FATAL_ERROR "readelf could not inspect ${BINARY}; see ${REPORT}")
  endif()
  if(MODE STREQUAL "static")
    if(ELF_PROGRAM_HEADERS MATCHES "(^|[ \t])INTERP([ \t]|$)")
      message(FATAL_ERROR
        "Fully static Linux binary contains an ELF INTERP program header; see ${REPORT}")
    endif()
    if(ELF_DYNAMIC MATCHES "NEEDED")
      message(FATAL_ERROR
        "Fully static Linux binary contains a dynamic NEEDED entry; see ${REPORT}")
    endif()
    if(NOT CROSSCOMPILING AND LDD_TOOL AND NOT LDD_TOOL MATCHES "-NOTFOUND$")
      run_tool(LDD_OUTPUT "ldd" "${LDD_TOOL}" "${BINARY}")
      string(TOLOWER "${LDD_OUTPUT}" LDD_LOWER)
      if(NOT LDD_LOWER MATCHES "not a dynamic executable|statically linked")
        message(FATAL_ERROR "ldd did not identify ${BINARY} as static; see ${REPORT}")
      endif()
    else()
      file(APPEND "${REPORT}" "\nldd: skipped for cross build or unavailable\n")
    endif()
  else()
    reject_crypto_dependencies("${ELF_DYNAMIC}")
  endif()
elseif(TARGET_SYSTEM STREQUAL "Darwin")
  if(MODE STREQUAL "static")
    message(FATAL_ERROR "Fully static linking is not supported on macOS. Use mostly-static instead.")
  endif()
  if(NOT OTOOL_TOOL OR OTOOL_TOOL MATCHES "-NOTFOUND$")
    message(FATAL_ERROR "otool is required to verify macOS link dependencies")
  endif()
  run_tool(MACHO_DEPENDENCIES "otool dependencies" "${OTOOL_TOOL}" -L "${BINARY}")
  if(NOT MACHO_DEPENDENCIES_RESULT EQUAL 0)
    message(FATAL_ERROR "otool could not inspect ${BINARY}; see ${REPORT}")
  endif()
  reject_crypto_dependencies("${MACHO_DEPENDENCIES}")
elseif(TARGET_SYSTEM STREQUAL "Windows")
  if(DUMPBIN_TOOL AND NOT DUMPBIN_TOOL MATCHES "-NOTFOUND$")
    run_tool(WINDOWS_DEPENDENCIES "dumpbin dependents" "${DUMPBIN_TOOL}" /dependents "${BINARY}")
  elseif(OBJDUMP_TOOL AND NOT OBJDUMP_TOOL MATCHES "-NOTFOUND$")
    run_tool(WINDOWS_DEPENDENCIES "objdump PE headers" "${OBJDUMP_TOOL}" -p "${BINARY}")
  else()
    message(FATAL_ERROR
      "dumpbin or objdump is required to verify Windows link dependencies")
  endif()
  if(NOT WINDOWS_DEPENDENCIES_RESULT EQUAL 0)
    message(FATAL_ERROR "Windows dependency inspection failed; see ${REPORT}")
  endif()
  reject_crypto_dependencies("${WINDOWS_DEPENDENCIES}")
  if(MODE STREQUAL "static")
    string(TOLOWER "${WINDOWS_DEPENDENCIES}" WINDOWS_DEPENDENCIES_LOWER)
    if(WINDOWS_DEPENDENCIES_LOWER MATCHES
        "vcruntime[0-9]*\\.dll|msvcp[0-9]*\\.dll|msvcr[0-9]*\\.dll|ucrtbase\\.dll|libgcc_s.*\\.dll|libwinpthread.*\\.dll")
      message(FATAL_ERROR
        "Windows static mode still depends on a dynamic compiler/CRT runtime; see ${REPORT}")
    endif()
  endif()
else()
  file(APPEND "${REPORT}" "\ndependency verification: unsupported target system\n")
endif()

message(STATUS "Verified ${MODE} link mode; report: ${REPORT}")
