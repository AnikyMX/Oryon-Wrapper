cmake_minimum_required(VERSION 3.22)
project(oryon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(ORYON_HOST_TEST "Build the host smoke test (non-Android)" OFF)

file(GLOB_RECURSE ORYON_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_library(oryon SHARED ${ORYON_SOURCES})
target_include_directories(oryon PRIVATE src third_party/vulkan)
target_compile_options(oryon PRIVATE -fno-exceptions -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden
                                     -ffunction-sections -fdata-sections -Wall -Wextra)
target_link_options(oryon PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--no-undefined
                                  -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/oryon.map
                                  $<$<CONFIG:Release>:-Wl,--strip-all>)
set_target_properties(oryon PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                                       LINK_DEPENDS ${CMAKE_SOURCE_DIR}/src/oryon.map)
if(ANDROID)
  target_link_libraries(oryon PRIVATE android log dl)
else()
  target_link_libraries(oryon PRIVATE dl pthread)
endif()

if(ORYON_HOST_TEST AND NOT ANDROID)
  add_executable(oryon_smoke tests/host/smoke.cpp)
  target_include_directories(oryon_smoke PRIVATE src)
  target_compile_options(oryon_smoke PRIVATE -Wall -Wextra)
  target_link_libraries(oryon_smoke PRIVATE dl)
  add_dependencies(oryon_smoke oryon)
endif()
