cmake_minimum_required(VERSION 3.25)
# Finalverse Map Server: a native gateway in front of an unmodified MapServer
# backend. It is built separately from MapServer and never patches it.
project(FinalverseMapServer VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

option(FVMAP_SANITIZERS "Address and undefined behavior sanitizers" OFF)
option(FVMAP_FUZZ "Build libFuzzer targets (needs LLVM clang)" OFF)
option(FVMAP_WITH_MUTSEA "World API backed by the MutSea native read projection" OFF)

find_package(Boost 1.84 CONFIG REQUIRED COMPONENTS json url)
find_package(OpenSSL 3 REQUIRED)
find_package(GDAL 3.8 CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(fvmap_sources
  src/util.cpp src/config.cpp src/auth.cpp src/catalog.cpp src/wms.cpp
  src/backend.cpp src/cache.cpp src/metrics.cpp src/spatial.cpp src/world.cpp
  src/server.cpp)
add_library(fvmap_core STATIC ${fvmap_sources})
target_include_directories(fvmap_core PUBLIC src)
target_link_libraries(fvmap_core PUBLIC Boost::json Boost::url OpenSSL::Crypto GDAL::GDAL Threads::Threads)
target_compile_options(fvmap_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(fvmap_core PUBLIC BOOST_ASIO_NO_DEPRECATED)
if(FVMAP_WITH_MUTSEA)
  find_package(MutSeaNative 0.3 CONFIG REQUIRED)
  target_link_libraries(fvmap_core PUBLIC MutSea::mutsea_projection)
  target_compile_definitions(fvmap_core PUBLIC FVMAP_WITH_MUTSEA=1)
endif()
if(FVMAP_SANITIZERS)
  target_compile_options(fvmap_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(fvmap_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(fvmapd src/main.cpp)
target_link_libraries(fvmapd PRIVATE fvmap_core)

if(BUILD_TESTING)
  add_executable(fvmap_unit_tests tests/test_units.cpp)
  target_link_libraries(fvmap_unit_tests PRIVATE fvmap_core)
  target_compile_options(fvmap_unit_tests PRIVATE -UNDEBUG)
  add_test(NAME fvmap_units COMMAND fvmap_unit_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
  set(FVMAP_MAPSERV "" CACHE FILEPATH "mapserv CGI executable used by integration tests")
  set(FVMAP_SOURCE "" CACHE PATH "Montreal reference dataset directory used by integration tests")
  set(FVMAP_ENV_PREFIX "" CACHE PATH "Prefix providing share/proj for the backend")
  set(FVMAP_WORLD_STORE "" CACHE FILEPATH "Copied MutSea region store for world API tests")
  if(FVMAP_MAPSERV AND FVMAP_SOURCE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME fvmap_integration COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
      --fvmapd $<TARGET_FILE:fvmapd> --mapserv ${FVMAP_MAPSERV} --source ${FVMAP_SOURCE}
      --env-prefix "${FVMAP_ENV_PREFIX}" --world-store "${FVMAP_WORLD_STORE}")
    set_tests_properties(fvmap_integration PROPERTIES TIMEOUT 600)
  endif()
endif()

if(FVMAP_FUZZ)
  add_executable(fvmap_fuzz_wms tests/fuzz_wms.cpp)
  target_link_libraries(fvmap_fuzz_wms PRIVATE fvmap_core)
  target_compile_options(fvmap_fuzz_wms PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fvmap_fuzz_wms PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(TARGETS fvmapd RUNTIME DESTINATION bin)
install(FILES config/montreal.example.json DESTINATION share/finalverse-map-server)
