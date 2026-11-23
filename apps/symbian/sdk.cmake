if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdk-location.json")
  file(READ "${CMAKE_CURRENT_LIST_DIR}/sdk-location.json" sdk_selection)
  string(JSON sdk_directory GET "${sdk_selection}" sdk)
  get_filename_component(SYMBIAN_SDK_PREFIX "${sdk_directory}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
elseif(DEFINED ENV{SYMBIAN_SDK_MANIFEST})
  get_filename_component(SYMBIAN_SDK_PREFIX "$ENV{SYMBIAN_SDK_MANIFEST}" DIRECTORY)
endif()

if(NOT EXISTS "${SYMBIAN_SDK_PREFIX}/cmake/symbian-arm.cmake")
  message(FATAL_ERROR "Set SYMBIAN_SDK_MANIFEST to the path to the Symbian SDK's sdk.json.")
endif()

include("${SYMBIAN_SDK_PREFIX}/cmake/symbian-arm.cmake")
