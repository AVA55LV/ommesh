cmake_minimum_required(VERSION 3.28)
project(ommesh_converter_build LANGUAGES CXX)

set(OMMESH_SDK_SOURCES "" CACHE PATH "Symbian Platform source checkout")
find_program(PATCH_EXECUTABLE NAMES patch REQUIRED)
if(NOT IS_DIRECTORY "${OMMESH_SDK_SOURCES}/cpp/symbian/e32")
  message(FATAL_ERROR "Set OMMESH_SDK_SOURCES to the Symbian Platform checkout from README.md.")
endif()

set(sources "${CMAKE_CURRENT_BINARY_DIR}/sources")
set(modules analysis sdk e32 tools)
foreach(module IN LISTS modules)
  file(COPY "${OMMESH_SDK_SOURCES}/cpp/symbian/${module}" DESTINATION "${sources}/cpp/symbian")
endforeach()
configure_file(Standalone.cmake "${sources}/CMakeLists.txt" COPYONLY)
execute_process(
  COMMAND "${PATCH_EXECUTABLE}" --batch --forward -p1 -i "${CMAKE_CURRENT_SOURCE_DIR}/imports.patch"
  WORKING_DIRECTORY "${sources}"
  COMMAND_ERROR_IS_FATAL ANY
)
add_subdirectory("${sources}" "${CMAKE_CURRENT_BINARY_DIR}/converter")
