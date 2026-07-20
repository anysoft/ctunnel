cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(ctunnel VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(CTUNNEL_KCONFIG_CONFIG "$ENV{KCONFIG_CONFIG}" CACHE FILEPATH
    "Path to the Kconfig .config file")
if(NOT CTUNNEL_KCONFIG_CONFIG)
  set(CTUNNEL_KCONFIG_CONFIG "${CMAKE_SOURCE_DIR}/.config" CACHE FILEPATH "" FORCE)
endif()
set(CTUNNEL_DEFCONFIG "" CACHE FILEPATH "Seed defconfig used only when .config is absent")

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(CTUNNEL_KCONFIG_TARGET windows)
  set(CTUNNEL_PLATFORM_DEFCONFIG configs/windows_defconfig)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CTUNNEL_KCONFIG_TARGET darwin)
  set(CTUNNEL_PLATFORM_DEFCONFIG configs/macos_defconfig)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CTUNNEL_KCONFIG_TARGET linux)
  set(CTUNNEL_PLATFORM_DEFCONFIG configs/linux_defconfig)
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" CTUNNEL_KCONFIG_TARGET)
  set(CTUNNEL_PLATFORM_DEFCONFIG configs/default_defconfig)
endif()

if(NOT IS_ABSOLUTE "${CTUNNEL_KCONFIG_CONFIG}")
  get_filename_component(CTUNNEL_KCONFIG_CONFIG "${CTUNNEL_KCONFIG_CONFIG}" ABSOLUTE
                         BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()
if(IS_DIRECTORY "${CTUNNEL_KCONFIG_CONFIG}")
  set(CTUNNEL_KCONFIG_CONFIG "${CTUNNEL_KCONFIG_CONFIG}/.config" CACHE FILEPATH "" FORCE)
endif()
if(EXISTS "${CTUNNEL_KCONFIG_CONFIG}")
  set(CTUNNEL_KCONFIG_ACTION generate)
else()
  if(CTUNNEL_DEFCONFIG)
    set(CTUNNEL_KCONFIG_SEED "${CTUNNEL_DEFCONFIG}")
  else()
    set(CTUNNEL_KCONFIG_SEED "${CMAKE_SOURCE_DIR}/${CTUNNEL_PLATFORM_DEFCONFIG}")
  endif()
  set(CTUNNEL_KCONFIG_ACTION defconfig "${CTUNNEL_KCONFIG_SEED}")
  message(STATUS "No .config found; seeding ${CTUNNEL_KCONFIG_CONFIG} from ${CTUNNEL_KCONFIG_SEED}")
endif()
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/kconfig/configure.py"
          --config "${CTUNNEL_KCONFIG_CONFIG}" --build-dir "${CMAKE_BINARY_DIR}"
          --header "${CMAKE_BINARY_DIR}/generated/include/generated/autoconf.h"
          --target "${CTUNNEL_KCONFIG_TARGET}" ${CTUNNEL_KCONFIG_ACTION}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE CTUNNEL_KCONFIG_RESULT
  OUTPUT_VARIABLE CTUNNEL_KCONFIG_STDOUT
  ERROR_VARIABLE CTUNNEL_KCONFIG_STDERR)
if(NOT CTUNNEL_KCONFIG_RESULT EQUAL 0)
  message(FATAL_ERROR "Kconfig generation failed:\n${CTUNNEL_KCONFIG_STDOUT}${CTUNNEL_KCONFIG_STDERR}")
endif()
include("${CMAKE_BINARY_DIR}/generated/config.cmake")

set(CTUNNEL_LINK_MODE_COUNT 0)
foreach(CTUNNEL_LINK_SYMBOL
        CONFIG_CTUNNEL_LINK_DYNAMIC
        CONFIG_CTUNNEL_LINK_MOSTLY_STATIC
        CONFIG_CTUNNEL_LINK_STATIC)
  if(${CTUNNEL_LINK_SYMBOL})
    math(EXPR CTUNNEL_LINK_MODE_COUNT "${CTUNNEL_LINK_MODE_COUNT} + 1")
  endif()
endforeach()
if(NOT CTUNNEL_LINK_MODE_COUNT EQUAL 1)
  message(FATAL_ERROR "Exactly one ctunnel link mode must be enabled")
endif()
if(CONFIG_CTUNNEL_LINK_DYNAMIC)
  set(CTUNNEL_LINK_MODE dynamic)
elseif(CONFIG_CTUNNEL_LINK_MOSTLY_STATIC)
  set(CTUNNEL_LINK_MODE mostly-static)
else()
  set(CTUNNEL_LINK_MODE static)
endif()

if(APPLE AND CONFIG_CTUNNEL_LINK_STATIC)
  message(FATAL_ERROR
    "Fully static linking is not supported on macOS.\nUse mostly-static instead.")
endif()

if(MSVC)
  if(CONFIG_CTUNNEL_LINK_STATIC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
endif()

set(CTUNNEL_TARGET_TRIPLET "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
    OUTPUT_VARIABLE CTUNNEL_COMPILER_TRIPLET
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(CTUNNEL_COMPILER_TRIPLET)
    set(CTUNNEL_TARGET_TRIPLET "${CTUNNEL_COMPILER_TRIPLET}")
  endif()
endif()

set(CTUNNEL_LIBC "auto" CACHE STRING
    "Target C library name for build reports (auto, glibc, musl, uClibc, ...)")
if(CTUNNEL_LIBC STREQUAL "auto")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckSymbolExists)
    unset(CTUNNEL_HAVE_GLIBC CACHE)
    unset(CTUNNEL_HAVE_GLIBC)
    unset(CTUNNEL_HAVE_UCLIBC CACHE)
    unset(CTUNNEL_HAVE_UCLIBC)
    check_symbol_exists(__GLIBC__ "features.h" CTUNNEL_HAVE_GLIBC)
    check_symbol_exists(__UCLIBC__ "features.h" CTUNNEL_HAVE_UCLIBC)
    if(CTUNNEL_HAVE_GLIBC)
      set(CTUNNEL_LIBC_RESOLVED glibc)
    elseif(CTUNNEL_HAVE_UCLIBC)
      set(CTUNNEL_LIBC_RESOLVED uClibc)
    elseif(CMAKE_C_COMPILER MATCHES "musl" OR CTUNNEL_TARGET_TRIPLET MATCHES "musl")
      set(CTUNNEL_LIBC_RESOLVED musl)
    else()
      set(CTUNNEL_LIBC_RESOLVED unknown-linux-libc)
    endif()
  elseif(APPLE)
    set(CTUNNEL_LIBC_RESOLVED libSystem)
  elseif(MSVC)
    set(CTUNNEL_LIBC_RESOLVED msvc-crt)
  elseif(MINGW)
    set(CTUNNEL_LIBC_RESOLVED mingw-crt)
  else()
    set(CTUNNEL_LIBC_RESOLVED unknown)
  endif()
else()
  set(CTUNNEL_LIBC_RESOLVED "${CTUNNEL_LIBC}")
endif()

if(CONFIG_CTUNNEL_LINK_STATIC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/CMakeFiles/ctunnel-static-probe")
  set(CTUNNEL_STATIC_PROBE_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
  if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" CTUNNEL_BUILD_TYPE_UPPER)
    string(APPEND CTUNNEL_STATIC_PROBE_FLAGS
           " ${CMAKE_C_FLAGS_${CTUNNEL_BUILD_TYPE_UPPER}} ${CMAKE_EXE_LINKER_FLAGS_${CTUNNEL_BUILD_TYPE_UPPER}}")
  endif()
  separate_arguments(CTUNNEL_STATIC_PROBE_FLAGS NATIVE_COMMAND "${CTUNNEL_STATIC_PROBE_FLAGS}")
  set(CTUNNEL_STATIC_PROBE_COMPILER_ARGS)
  if(CMAKE_C_COMPILER_ARG1)
    separate_arguments(CTUNNEL_STATIC_PROBE_COMPILER_ARGS NATIVE_COMMAND "${CMAKE_C_COMPILER_ARG1}")
  endif()
  if(CMAKE_C_COMPILER_TARGET)
    list(APPEND CTUNNEL_STATIC_PROBE_COMPILER_ARGS "--target=${CMAKE_C_COMPILER_TARGET}")
  endif()
  if(CMAKE_SYSROOT)
    list(APPEND CTUNNEL_STATIC_PROBE_COMPILER_ARGS "--sysroot=${CMAKE_SYSROOT}")
  endif()
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" ${CTUNNEL_STATIC_PROBE_COMPILER_ARGS}
            ${CTUNNEL_STATIC_PROBE_FLAGS} -static
            "${CMAKE_SOURCE_DIR}/cmake/static-link-probe.c"
            -o "${CMAKE_BINARY_DIR}/CMakeFiles/ctunnel-static-probe/probe"
    RESULT_VARIABLE CTUNNEL_STATIC_LINK_RESULT
    OUTPUT_VARIABLE CTUNNEL_STATIC_LINK_PROBE_STDOUT
    ERROR_VARIABLE CTUNNEL_STATIC_LINK_PROBE_STDERR)
  if(NOT CTUNNEL_STATIC_LINK_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Fully static linking was requested, but the target toolchain could not link a "
      "static executable. Ensure the target SDK sysroot contains its static libc and "
      "runtime archives; do not use host libc archives for a cross build.\n"
      "Static-link probe output:\n${CTUNNEL_STATIC_LINK_PROBE_STDOUT}${CTUNNEL_STATIC_LINK_PROBE_STDERR}")
  endif()
  if(CTUNNEL_LIBC_RESOLVED STREQUAL "glibc")
    message(WARNING
      "Fully static glibc binaries can be significantly larger and may have NSS/DNS "
      "runtime limitations. Mostly-static is normally preferable for glibc devices.")
  else()
    message(WARNING
      "Fully static linking is enabled. Confirm that every archive comes from the "
      "target SDK sysroot; the post-link verifier will reject an ELF interpreter or NEEDED entry.")
  endif()
endif()
file(GLOB_RECURSE CTUNNEL_KCONFIG_FILES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/Kconfig" "${CMAKE_SOURCE_DIR}/src/Kconfig"
     "${CMAKE_SOURCE_DIR}/src/*/Kconfig")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CTUNNEL_KCONFIG_CONFIG}" ${CTUNNEL_KCONFIG_FILES}
             "${CMAKE_SOURCE_DIR}/scripts/kconfig/configure.py"
             "${CMAKE_SOURCE_DIR}/scripts/kconfig/ctkconfig.py")

execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE CTUNNEL_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT CTUNNEL_GIT_COMMIT)
  set(CTUNNEL_GIT_COMMIT "unknown")
endif()
execute_process(COMMAND git describe --tags --exact-match HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE CTUNNEL_EXACT_TAG OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(CTUNNEL_VERSION_OVERRIDE "" CACHE STRING "Override ctunnel runtime/package version string")
if(CTUNNEL_VERSION_OVERRIDE)
  set(CTUNNEL_VERSION_STRING "${CTUNNEL_VERSION_OVERRIDE}")
elseif(CTUNNEL_EXACT_TAG MATCHES "^v(.+)$")
  set(CTUNNEL_VERSION_STRING "${CMAKE_MATCH_1}")
else()
  set(CTUNNEL_VERSION_STRING "${PROJECT_VERSION}-dev+${CTUNNEL_GIT_COMMIT}")
endif()

option(CTUNNEL_BUILD_TESTS "Build unit and integration tests" ON)
option(CTUNNEL_BUILD_FUZZERS "Build libFuzzer parser targets (Clang only)" OFF)
option(CTUNNEL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CTUNNEL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CTUNNEL_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(CTUNNEL_USE_BUNDLED_MONOCYPHER "Use bundled Monocypher source" ON)
set(CTUNNEL_MONOCYPHER_ROOT "" CACHE PATH "External Monocypher source root")

if(NOT CONFIG_CRYPTO_MONOCYPHER OR NOT CONFIG_CRYPTO_CHACHA20_POLY1305)
  message(FATAL_ERROR "Phase 1 requires the Monocypher XChaCha20-Poly1305 backend")
endif()
if(CTUNNEL_USE_BUNDLED_MONOCYPHER)
  set(CTUNNEL_MONOCYPHER_SOURCE "${CMAKE_SOURCE_DIR}/third_party/monocypher/src/monocypher.c")
  set(CTUNNEL_MONOCYPHER_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/monocypher/include")
else()
  if(NOT CTUNNEL_MONOCYPHER_ROOT)
    message(FATAL_ERROR "Set CTUNNEL_MONOCYPHER_ROOT when bundled Monocypher is disabled")
  endif()
  set(CTUNNEL_MONOCYPHER_SOURCE "${CTUNNEL_MONOCYPHER_ROOT}/src/monocypher.c")
  if(EXISTS "${CTUNNEL_MONOCYPHER_ROOT}/include/monocypher.h")
    set(CTUNNEL_MONOCYPHER_INCLUDE "${CTUNNEL_MONOCYPHER_ROOT}/include")
  else()
    set(CTUNNEL_MONOCYPHER_INCLUDE "${CTUNNEL_MONOCYPHER_ROOT}/src")
  endif()
endif()
if(NOT EXISTS "${CTUNNEL_MONOCYPHER_SOURCE}" OR
   NOT EXISTS "${CTUNNEL_MONOCYPHER_INCLUDE}/monocypher.h")
  message(FATAL_ERROR "Monocypher source/header not found at the selected root")
endif()

add_library(ctunnel_monocypher STATIC "${CTUNNEL_MONOCYPHER_SOURCE}")
target_include_directories(ctunnel_monocypher PUBLIC "${CTUNNEL_MONOCYPHER_INCLUDE}")
if(CONFIG_OPTIMIZE_FOR_SIZE)
  target_compile_definitions(ctunnel_monocypher PRIVATE BLAKE2_NO_UNROLLING)
endif()

set(CTUNNEL_SOURCES
  src/applets/main.c
  src/applets/common.c
  src/applets/legacy.c
  src/applets/version.c
  src/app/runtime.c
  src/config/config.c
  src/protocol/protocol.c
  src/crypto/crypto_monocypher.c
  src/net/net.c
  src/net/relay.c
  src/platform/platform.c
  src/util/log.c
  src/util/ring.c)

if(CONFIG_FEATURE_PROTOCOL_DIAGNOSTICS)
  list(APPEND CTUNNEL_SOURCES src/protocol/diagnostics.c)
endif()

if(CONFIG_CTUNNEL_SERVER)
  list(APPEND CTUNNEL_SOURCES src/server/server.c src/applets/server_applet.c)
endif()
if(CONFIG_CTUNNEL_CLIENT)
  list(APPEND CTUNNEL_SOURCES src/client/client.c src/applets/client_applet.c)
endif()
if(CONFIG_FEATURE_KEYGEN)
  list(APPEND CTUNNEL_SOURCES src/applets/keygen_applet.c)
endif()
if(CONFIG_FEATURE_FINGERPRINT)
  list(APPEND CTUNNEL_SOURCES src/applets/fingerprint_applet.c)
endif()
if(CONFIG_FEATURE_CONFIG_TEST)
  list(APPEND CTUNNEL_SOURCES src/applets/configtest_applet.c)
endif()
if(CONFIG_FEATURE_BUILD_INFO)
  list(APPEND CTUNNEL_SOURCES src/applets/build_info_applet.c)
endif()

if(CONFIG_EVENT_EPOLL)
  list(APPEND CTUNNEL_SOURCES src/platform/event_epoll.c)
  set(CTUNNEL_EVENT_NAME epoll)
elseif(CONFIG_EVENT_KQUEUE)
  list(APPEND CTUNNEL_SOURCES src/platform/event_kqueue.c)
  set(CTUNNEL_EVENT_NAME kqueue)
elseif(CONFIG_EVENT_WSAPOLL)
  list(APPEND CTUNNEL_SOURCES src/platform/event_poll.c)
  set(CTUNNEL_EVENT_NAME wsapoll)
elseif(CONFIG_EVENT_POLL)
  list(APPEND CTUNNEL_SOURCES src/platform/event_poll.c)
  set(CTUNNEL_EVENT_NAME poll)
else()
  message(FATAL_ERROR "No event backend selected")
endif()

add_executable(ctunnel ${CTUNNEL_SOURCES})
target_include_directories(ctunnel PRIVATE "${CMAKE_BINARY_DIR}/generated/include" include src)
target_link_libraries(ctunnel PRIVATE ctunnel_monocypher)
target_compile_definitions(ctunnel PRIVATE
  CTUNNEL_VERSION="${CTUNNEL_VERSION_STRING}"
  CTUNNEL_GIT_COMMIT="${CTUNNEL_GIT_COMMIT}"
  CTUNNEL_TARGET="${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
  CTUNNEL_EVENT_NAME="${CTUNNEL_EVENT_NAME}"
  CTUNNEL_LINK_MODE="${CTUNNEL_LINK_MODE}"
  CTUNNEL_LIBC_NAME="${CTUNNEL_LIBC_RESOLVED}"
  CTUNNEL_CONFIG_HASH="${CTUNNEL_CONFIG_HASH}")

if(WIN32)
  target_link_libraries(ctunnel PRIVATE ws2_32 bcrypt)
endif()

if(CONFIG_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT CTUNNEL_IPO_OK OUTPUT CTUNNEL_IPO_ERROR)
  if(NOT CTUNNEL_IPO_OK)
    message(FATAL_ERROR "CONFIG_ENABLE_LTO requested but unsupported: ${CTUNNEL_IPO_ERROR}")
  endif()
  set_property(TARGET ctunnel ctunnel_monocypher PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(MSVC)
  target_compile_options(ctunnel PRIVATE /W4)
  if(CONFIG_OPTIMIZE_FOR_SIZE)
    target_compile_options(ctunnel PRIVATE /O1)
  endif()
else()
  set(CTUNNEL_PROJECT_WARNINGS
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2
      -Wformat-security -Wnull-dereference -Wdouble-promotion -Wstrict-prototypes
      -Wmissing-prototypes -Wcast-align -Wundef -Wwrite-strings)
  target_compile_options(ctunnel PRIVATE ${CTUNNEL_PROJECT_WARNINGS} -fvisibility=hidden)
  if(CTUNNEL_WARNINGS_AS_ERRORS)
    target_compile_options(ctunnel PRIVATE -Werror)
  endif()
  target_compile_options(ctunnel_monocypher PRIVATE -fvisibility=hidden)
  if(CONFIG_OPTIMIZE_FOR_SIZE)
    target_compile_options(ctunnel PRIVATE -Os)
    target_compile_options(ctunnel_monocypher PRIVATE -Os)
  endif()
  if(CONFIG_ENABLE_SECTION_GC)
    target_compile_options(ctunnel PRIVATE -ffunction-sections -fdata-sections)
    target_compile_options(ctunnel_monocypher PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(ctunnel PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(ctunnel PRIVATE -Wl,--gc-sections)
    endif()
  endif()
  if(CTUNNEL_ENABLE_ASAN)
    target_compile_options(ctunnel PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_compile_options(ctunnel_monocypher PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(ctunnel PRIVATE -fsanitize=address)
  endif()
  if(CTUNNEL_ENABLE_UBSAN)
    target_compile_options(ctunnel PRIVATE -fsanitize=undefined)
    target_compile_options(ctunnel_monocypher PRIVATE -fsanitize=undefined)
    target_link_options(ctunnel PRIVATE -fsanitize=undefined)
  endif()
  if(CONFIG_CTUNNEL_LINK_STATIC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ctunnel PRIVATE -static)
  endif()
  if(CONFIG_CTUNNEL_LINK_STATIC AND MINGW)
    target_link_options(ctunnel PRIVATE -static -static-libgcc)
  endif()
endif()

message(STATUS "ctunnel configuration: ${CTUNNEL_KCONFIG_CONFIG} (${CTUNNEL_CONFIG_HASH})")
if(CONFIG_CTUNNEL_CLIENT AND CONFIG_CTUNNEL_SERVER)
  set(CTUNNEL_ROLE_SUMMARY "client+server")
elseif(CONFIG_CTUNNEL_CLIENT)
  set(CTUNNEL_ROLE_SUMMARY "client-only")
else()
  set(CTUNNEL_ROLE_SUMMARY "server-only")
endif()
message(STATUS "  role=${CTUNNEL_ROLE_SUMMARY} event=${CTUNNEL_EVENT_NAME} IPv4=${CONFIG_FEATURE_IPV4} IPv6=${CONFIG_FEATURE_IPV6}")
message(STATUS "  link=${CTUNNEL_LINK_MODE} libc=${CTUNNEL_LIBC_RESOLVED} triplet=${CTUNNEL_TARGET_TRIPLET}")
message(STATUS "  data-encryption=${CONFIG_FEATURE_DATA_ENCRYPTION} work-pool=${CONFIG_FEATURE_WORK_POOL} build-info=${CONFIG_FEATURE_BUILD_INFO}")
message(STATUS "  limits: clients=${CONFIG_MAX_CLIENTS} services=${CONFIG_MAX_SERVICES} streams=${CONFIG_MAX_STREAMS} pending=${CONFIG_MAX_PENDING_STREAMS}")

find_program(CTUNNEL_FILE_TOOL NAMES file)
find_program(CTUNNEL_LDD_TOOL NAMES ldd)
find_program(CTUNNEL_OTOOL_TOOL NAMES otool)
find_program(CTUNNEL_DUMPBIN_TOOL NAMES dumpbin)
if(CMAKE_READELF)
  set(CTUNNEL_READELF_TOOL "${CMAKE_READELF}")
else()
  find_program(CTUNNEL_READELF_TOOL NAMES readelf llvm-readelf)
endif()
if(CMAKE_OBJDUMP)
  set(CTUNNEL_OBJDUMP_TOOL "${CMAKE_OBJDUMP}")
else()
  find_program(CTUNNEL_OBJDUMP_TOOL NAMES objdump llvm-objdump)
endif()
if(CMAKE_SIZE)
  set(CTUNNEL_SIZE_TOOL "${CMAKE_SIZE}")
else()
  find_program(CTUNNEL_SIZE_TOOL NAMES size llvm-size)
endif()

add_custom_command(TARGET ctunnel POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
          "-DBINARY=$<TARGET_FILE:ctunnel>"
          "-DREPORT=$<TARGET_FILE:ctunnel>.link-report.txt"
          "-DMODE=${CTUNNEL_LINK_MODE}"
          "-DTARGET_SYSTEM=${CMAKE_SYSTEM_NAME}"
          "-DTARGET_TRIPLET=${CTUNNEL_TARGET_TRIPLET}"
          "-DLIBC=${CTUNNEL_LIBC_RESOLVED}"
          "-DCOMPILER_ID=${CMAKE_C_COMPILER_ID}"
          "-DCOMPILER_VERSION=${CMAKE_C_COMPILER_VERSION}"
          "-DCOMPILER_PATH=${CMAKE_C_COMPILER}"
          "-DCROSSCOMPILING=${CMAKE_CROSSCOMPILING}"
          "-DFILE_TOOL=${CTUNNEL_FILE_TOOL}"
          "-DREADELF_TOOL=${CTUNNEL_READELF_TOOL}"
          "-DLDD_TOOL=${CTUNNEL_LDD_TOOL}"
          "-DOTOOL_TOOL=${CTUNNEL_OTOOL_TOOL}"
          "-DDUMPBIN_TOOL=${CTUNNEL_DUMPBIN_TOOL}"
          "-DOBJDUMP_TOOL=${CTUNNEL_OBJDUMP_TOOL}"
          "-DSIZE_TOOL=${CTUNNEL_SIZE_TOOL}"
          -P "${CMAKE_SOURCE_DIR}/cmake/VerifyLinkMode.cmake"
  COMMENT "Verifying ${CTUNNEL_LINK_MODE} link mode"
  VERBATIM)
set_property(TARGET ctunnel APPEND PROPERTY LINK_DEPENDS
             "${CMAKE_SOURCE_DIR}/cmake/VerifyLinkMode.cmake")

add_custom_target(size-check
  COMMAND ${CMAKE_COMMAND} -E env
          CTUNNEL_MAX_MINI_BINARY_SIZE_KIB=${CONFIG_MINI_MAX_BINARY_SIZE_KIB}
          CTUNNEL_FILE=${CTUNNEL_FILE_TOOL}
          CTUNNEL_STRIP=${CMAKE_STRIP}
          CTUNNEL_SIZE=${CTUNNEL_SIZE_TOOL}
          CTUNNEL_NM=${CMAKE_NM}
          CTUNNEL_READELF=${CTUNNEL_READELF_TOOL}
          CTUNNEL_OBJDUMP=${CTUNNEL_OBJDUMP_TOOL}
          CTUNNEL_LINK_MODE=${CTUNNEL_LINK_MODE}
          CTUNNEL_LIBC=${CTUNNEL_LIBC_RESOLVED}
          CTUNNEL_COMPILER=${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}
          CTUNNEL_TARGET_TRIPLET=${CTUNNEL_TARGET_TRIPLET}
          ${CMAKE_SOURCE_DIR}/scripts/size-report.sh $<TARGET_FILE:ctunnel>
  DEPENDS ctunnel USES_TERMINAL)

include(GNUInstallDirs)
install(TARGETS ctunnel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES examples/server.ini examples/client.ini examples/clients.ini
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/ctunnel/examples)

if(CTUNNEL_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CTUNNEL_BUILD_FUZZERS requires Clang")
  endif()
  foreach(fuzz_target frame_fuzz handshake_fuzz data_record_fuzz service_registration_fuzz)
    add_executable(${fuzz_target} tests/fuzz/${fuzz_target}.c src/protocol/protocol.c)
    if(CONFIG_FEATURE_PROTOCOL_DIAGNOSTICS)
      target_sources(${fuzz_target} PRIVATE src/protocol/diagnostics.c src/util/log.c)
    endif()
    target_include_directories(${fuzz_target} PRIVATE "${CMAKE_BINARY_DIR}/generated/include" include src)
    target_compile_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
  add_executable(config_fuzz tests/fuzz/config_fuzz.c src/config/config.c src/util/log.c)
  target_include_directories(config_fuzz PRIVATE "${CMAKE_BINARY_DIR}/generated/include" include src)
  target_compile_options(config_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(config_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(key_file_fuzz tests/fuzz/key_file_fuzz.c src/crypto/crypto_monocypher.c
    src/platform/platform.c src/protocol/protocol.c)
  target_include_directories(key_file_fuzz PRIVATE "${CMAKE_BINARY_DIR}/generated/include" include src)
  target_link_libraries(key_file_fuzz PRIVATE ctunnel_monocypher)
  target_compile_options(key_file_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(key_file_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(CTUNNEL_BUILD_TESTS)
  enable_testing()
  add_test(NAME config-system
           COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/config/test_kconfig.py")
  add_test(NAME protocol-vectors
           COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/vectors/reference_vectors.py"
                   --check "${CMAKE_SOURCE_DIR}/tests/vectors/v3.json")
  if(NOT CONFIG_FEATURE_KEYGEN OR NOT CONFIG_FEATURE_FINGERPRINT OR
     NOT CONFIG_FEATURE_DATA_ENCRYPTION)
    message(STATUS "Unit tests disabled: this trimmed configuration omits test dependencies")
  else()
    add_executable(ctunnel_tests tests/unit/test_main.c
      src/config/config.c src/protocol/protocol.c src/crypto/crypto_monocypher.c
      src/net/net.c src/net/relay.c src/util/ring.c src/platform/platform.c src/util/log.c)
    if(CONFIG_FEATURE_PROTOCOL_DIAGNOSTICS)
      target_sources(ctunnel_tests PRIVATE src/protocol/diagnostics.c)
    endif()
    target_include_directories(ctunnel_tests PRIVATE "${CMAKE_BINARY_DIR}/generated/include" include src)
    target_link_libraries(ctunnel_tests PRIVATE ctunnel_monocypher)
    if(NOT MSVC)
      target_compile_options(ctunnel_tests PRIVATE ${CTUNNEL_PROJECT_WARNINGS})
      if(CTUNNEL_WARNINGS_AS_ERRORS)
        target_compile_options(ctunnel_tests PRIVATE -Werror)
      endif()
    endif()
    if(WIN32)
      target_link_libraries(ctunnel_tests PRIVATE ws2_32 bcrypt)
    endif()
    if(CTUNNEL_ENABLE_ASAN AND NOT MSVC)
      target_compile_options(ctunnel_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(ctunnel_tests PRIVATE -fsanitize=address)
    endif()
    if(CTUNNEL_ENABLE_UBSAN AND NOT MSVC)
      target_compile_options(ctunnel_tests PRIVATE -fsanitize=undefined)
      target_link_options(ctunnel_tests PRIVATE -fsanitize=undefined)
    endif()
    add_test(NAME unit COMMAND ctunnel_tests)
    if(NOT WIN32 AND CONFIG_CTUNNEL_CLIENT AND CONFIG_CTUNNEL_SERVER)
      add_test(NAME integration COMMAND sh ${CMAKE_SOURCE_DIR}/scripts/integration-test.sh $<TARGET_FILE:ctunnel>)
      set_tests_properties(integration PROPERTIES TIMEOUT 60)
    endif()
  endif()
endif()
