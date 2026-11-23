cmake_minimum_required(VERSION 3.28)
project(ommesh_e32_host_tool LANGUAGES CXX)

include(FetchContent)
set(CMAKE_CXX_STANDARD 20)
set(ABSL_ENABLE_INSTALL OFF)
set(ABSL_BUILD_TESTING OFF)
set(ABSL_PROPAGATE_CXX_STD ON)
FetchContent_Declare(abseil
  URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/5650e9cf76d3be4318d5fa3af38ee483ddfd5e4a
  URL_HASH SHA256=bac569e31429f85d7bdd9f90e4767442faf9c8fa1def08a66a7d5d78e0fba204
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(abseil)

# The SDK modules also run this hook when packaging a complete host SDK.
function(symbian_native_library target)
endfunction()

foreach(module IN ITEMS analysis sdk e32 tools)
  add_subdirectory(cpp/symbian/${module})
endforeach()
