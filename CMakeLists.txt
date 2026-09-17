cmake_minimum_required(VERSION 3.26)  # zvec source requires >= 3.26
# C is enabled for the in-tree zvec build (its vendored deps contain C sources).
project(zvmem CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# zvec linkage mode:
#   static (default): build zvec from source and link statically -> one fully
#                     self-contained binary, no .so to ship or find.
#   shared:           use the prebuilt SDK libzvec.so (fast builds; ships a .so).
# Select with -DZVMEM_ZVEC_MODE=shared.
# ---------------------------------------------------------------------------
set(ZVMEM_ZVEC_MODE "static" CACHE STRING
    "zvec linkage: 'static' (default, single self-contained binary) or 'shared' (prebuilt libzvec.so)")

if(ZVMEM_ZVEC_MODE STREQUAL "shared")
  # ------------------------- prebuilt SDK (downloaded, pinned) -------------
  set(ZVEC_VERSION "v0.7.0")
  set(ZVEC_ROOT "${CMAKE_BINARY_DIR}/third_party/zvec-sdk")

  if(NOT EXISTS "${ZVEC_ROOT}/lib/libzvec.so" AND NOT EXISTS "${ZVEC_ROOT}/lib/zvec.dll")
    set(ZVEC_TARBALL "${CMAKE_BINARY_DIR}/third_party/zvec-sdk.tar.gz")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(ZVEC_ASSET "zvec-sdk-linux-arm64.tar.gz")
      else()
        set(ZVEC_ASSET "zvec-sdk-linux-amd64.tar.gz")
      endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(ZVEC_ASSET "zvec-sdk-osx-arm64.tar.gz")
      else()
        message(FATAL_ERROR "macOS x86_64 zvec SDK asset not pinned yet (add zvec-sdk-osx-x64)")
      endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      set(ZVEC_ASSET "zvec-sdk-windows-amd64.zip")
    else()
      message(FATAL_ERROR "Unsupported platform for prebuilt zvec SDK: ${CMAKE_SYSTEM_NAME}")
    endif()

    file(DOWNLOAD
        "https://github.com/alibaba/zvec/releases/download/${ZVEC_VERSION}/${ZVEC_ASSET}"
        "${ZVEC_TARBALL}" SHOW_PROGRESS STATUS _zvec_dl)
    list(GET _zvec_dl 0 _zvec_dl_code)
    if(NOT _zvec_dl_code EQUAL 0)
      message(FATAL_ERROR "Failed to download zvec SDK: ${_zvec_dl}")
    endif()

    file(MAKE_DIRECTORY "${ZVEC_ROOT}")
    execute_process(COMMAND tar xzf "${ZVEC_TARBALL}" WORKING_DIRECTORY "${ZVEC_ROOT}"
                    RESULT_VARIABLE _zvec_tar_rc)
    if(NOT _zvec_tar_rc EQUAL 0)
      message(FATAL_ERROR "Failed to extract zvec SDK archive")
    endif()
  endif()

  find_library(ZVEC_LIB NAMES zvec PATHS "${ZVEC_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  add_library(zvec_sdk SHARED IMPORTED)
  set_target_properties(zvec_sdk PROPERTIES IMPORTED_LOCATION "${ZVEC_LIB}")
  target_include_directories(zvec_sdk INTERFACE "${ZVEC_ROOT}/include")

elseif(ZVMEM_ZVEC_MODE STREQUAL "static")
  # --------------------- build zvec from source, link statically -----------
  # Expect a v0.7.0 checkout (with submodules) at ./zvec-src by default:
  #   git clone https://github.com/alibaba/zvec.git zvec-src
  #   cd zvec-src && git checkout v0.7.0 && git submodule update --init --recursive
  set(ZVEC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zvec-src" CACHE PATH
      "Path to a zvec v0.7.0 source checkout (with submodules initialized)")
  if(NOT EXISTS "${ZVEC_SOURCE_DIR}/src/db/CMakeLists.txt")
    message(FATAL_ERROR
        "zvec source not found at ${ZVEC_SOURCE_DIR}.\n"
        "Clone it (pinned v0.7.0, with submodules):\n"
        "  git clone https://github.com/alibaba/zvec.git zvec-src\n"
        "  cd zvec-src && git checkout v0.7.0 && git submodule update --init --recursive\n"
        "...or point -DZVEC_SOURCE_DIR= at an existing checkout.")
  endif()

  # Skip the all-in-one shared-library packaging; we link the static targets.
  set(BUILD_ZVEC_SHARED OFF CACHE BOOL "" FORCE)
  add_subdirectory(${ZVEC_SOURCE_DIR} ${CMAKE_BINARY_DIR}/third_party/zvec EXCLUDE_FROM_ALL)

else()
  message(FATAL_ERROR "ZVMEM_ZVEC_MODE must be 'static' or 'shared', got '${ZVMEM_ZVEC_MODE}'")
endif()

# ---------------------------------------------------------------------------
# nlohmann/json (single header, pinned release)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)
add_library(json_iface INTERFACE)
target_include_directories(json_iface INTERFACE "${nlohmann_json_SOURCE_DIR}/include")

find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# zvmem executable
# ---------------------------------------------------------------------------
add_executable(zvmem
    src/main.cc
    src/config.cc
    src/embed_client.cc
    src/store.cc
)
target_link_libraries(zvmem PRIVATE json_iface CURL::libcurl)

if(ZVMEM_ZVEC_MODE STREQUAL "static")
  # zvec's indexers/converters self-register via static initializers (ailego
  # Factory pattern). Plain archive linking drops unreferenced object files,
  # which silently removes those registrations -> runtime "Create vector column
  # indexer failed". Force whole-archive inclusion of the internal libs so every
  # registration object is linked in (zvec's own MSVC build does /WHOLEARCHIVE).
  target_link_libraries(zvmem PRIVATE
      $<LINK_LIBRARY:WHOLE_ARCHIVE,zvec>
      $<LINK_LIBRARY:WHOLE_ARCHIVE,zvec_core>
      $<LINK_LIBRARY:WHOLE_ARCHIVE,zvec_ailego>
      $<LINK_LIBRARY:WHOLE_ARCHIVE,zvec_turbo>)
else()
  target_link_libraries(zvmem PRIVATE zvec_sdk)

  set_target_properties(zvmem PROPERTIES
      BUILD_RPATH "$ORIGIN;$ORIGIN/../third_party/zvec-sdk/lib"
      INSTALL_RPATH "$ORIGIN")

  # Ship the SDK library next to the binary so the tool is self-contained.
  add_custom_command(TARGET zvmem POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:zvmem>/lib
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ZVEC_LIB}" $<TARGET_FILE_DIR:zvmem>/lib/)
endif()
