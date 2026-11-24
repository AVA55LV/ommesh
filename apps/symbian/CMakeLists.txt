cmake_minimum_required(VERSION 3.28)
project(ommesh_belle LANGUAGES CXX ASM)

include(SymbianApp)
find_package(MbedTLS 3.4.1 EXACT CONFIG REQUIRED)

find_program(QT4_MOC_EXECUTABLE NAMES moc-qt4 moc4 moc REQUIRED)
execute_process(
  COMMAND "${QT4_MOC_EXECUTABLE}" -v
  OUTPUT_VARIABLE qt_moc_stdout
  ERROR_VARIABLE qt_moc_stderr
)
if(NOT "${qt_moc_stdout}${qt_moc_stderr}" MATCHES "Qt 4\\.8\\.")
  message(FATAL_ERROR "Set QT4_MOC_EXECUTABLE to a Qt 4.8 moc executable.")
endif()

get_filename_component(qt4_tool_directory "${QT4_MOC_EXECUTABLE}" DIRECTORY)
find_program(QT4_RCC_EXECUTABLE NAMES rcc-qt4 rcc4 rcc HINTS "${qt4_tool_directory}" REQUIRED)
execute_process(
  COMMAND "${QT4_RCC_EXECUTABLE}" -version
  OUTPUT_VARIABLE qt_rcc_stdout
  ERROR_VARIABLE qt_rcc_stderr
)
if(NOT "${qt_rcc_stdout}${qt_rcc_stderr}" MATCHES "version 4\\.8\\.")
  message(FATAL_ERROR "Set QT4_RCC_EXECUTABLE to a Qt 4.8 rcc executable.")
endif()

set(map_resource_dir "${CMAKE_CURRENT_SOURCE_DIR}/resources/maps")
set(map_resources "${CMAKE_CURRENT_BINARY_DIR}/ommesh_maps.cpp")
add_custom_command(
  OUTPUT "${map_resources}"
  COMMAND "${QT4_RCC_EXECUTABLE}" -name ommesh_maps -no-compress
    -o "${map_resources}" "${map_resource_dir}/maps.qrc"
  DEPENDS "${map_resource_dir}/maps.qrc"
    "${map_resource_dir}/omsk.bin"
    "${map_resource_dir}/omsk-oblast.bin"
    "${map_resource_dir}/siberia.bin" "${map_resource_dir}/world.bin"
  VERBATIM
)

set(moc_sources)
foreach(class IN ITEMS client window chat_features node_map radio_settings native_wlan native_location phone_location link_preview network_filters network_node_card network_node_management settings_attachments settings_backup settings_radio_backup settings_radio_editor settings_radio_tools custom_keyboard)
  set(header "${CMAKE_CURRENT_SOURCE_DIR}/src/${class}.hpp")
  set(moc_source "${CMAKE_CURRENT_BINARY_DIR}/moc_${class}.cpp")
  add_custom_command(
    OUTPUT "${moc_source}"
    COMMAND "${QT4_MOC_EXECUTABLE}" -o "${moc_source}" "${header}"
    DEPENDS "${header}"
    VERBATIM
  )
  list(APPEND moc_sources "${moc_source}")
endforeach()

set(keyboard_layout_moc "${CMAKE_CURRENT_BINARY_DIR}/keyboard_layout.moc")
add_custom_command(
  OUTPUT "${keyboard_layout_moc}"
  COMMAND "${QT4_MOC_EXECUTABLE}" -o "${keyboard_layout_moc}" "${CMAKE_CURRENT_SOURCE_DIR}/src/keyboard_layout.cpp"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/keyboard_layout.cpp"
  VERBATIM
)
set_source_files_properties(src/keyboard_layout.cpp PROPERTIES OBJECT_DEPENDS "${keyboard_layout_moc}")

symbian_add_executable(ommesh_belle SOURCES
  src/main.cpp
  src/window.cpp
  src/client.cpp
  src/meshcore.cpp
  src/message_reaction.cpp
  src/chat_features.cpp
  src/link_preview.cpp
  src/node_map.cpp
  src/network_filters.cpp
  src/network_node_card.cpp
  src/network_node_management.cpp
  src/node_chat_options.cpp
  src/radio_console.cpp
  src/radio_settings.cpp
  src/settings_attachments.cpp
  src/settings_backup.cpp
  src/settings_radio_backup.cpp
  src/radio_backup_archive.cpp
  src/radio_key_identity.cpp
  src/backup_history_import.cpp
  src/settings_radio_editor.cpp
  src/settings_radio_tools.cpp
  src/native_vibration.cpp
  src/native_wlan.cpp
  src/native_location.cpp
  src/native_keyboard.cpp
  src/keyboard_layout.cpp
  src/custom_keyboard.cpp
  src/phone_location.cpp
  src/native_tile_https.cpp
  src/native_allocator.cpp
  src/native_echo.cpp
  src/demo_radio.cpp
)
# The SDK discovers native files recursively for registered applications.
# Keep standalone host test programs out of the native executable.
get_target_property(application_sources ommesh_belle SOURCES)
file(GLOB_RECURSE host_test_sources LIST_DIRECTORIES FALSE
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*"
)
list(REMOVE_ITEM application_sources ${host_test_sources})
set_property(TARGET ommesh_belle PROPERTY SOURCES ${application_sources})

target_sources(ommesh_belle PRIVATE ${moc_sources} "${map_resources}")
target_include_directories(ommesh_belle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# The E32 loader resolves Qt's imported metaobject and vtable pointers in
# read-only storage. Keep moc's const tables there while retaining relocations.
set_source_files_properties(${moc_sources} src/keyboard_layout.cpp PROPERTIES COMPILE_OPTIONS "-fno-pic;-fno-pie")
# Qt metatype registration keeps writable IDs with process lifetime. Use
# absolute relocations for those IDs so Belle can move the data independently.
set_source_files_properties(src/native_wlan.cpp PROPERTIES COMPILE_OPTIONS "-fno-pic;-fno-pie")
# Qt 4 rcc emits global initializer state in data; keep its references
# absolute so the Symbian loader can relocate the code and data separately.
set_source_files_properties("${map_resources}" PROPERTIES COMPILE_OPTIONS "-fno-pic;-fno-pie")
target_link_options(ommesh_belle PRIVATE -z notext)
# The firmware's Qt DLLs can delete objects created in this executable.
# Keep the SDK's C++ allocation operators, but use the same raw user heap.
target_link_options(ommesh_belle PRIVATE
  --wrap=SymbianRuntimeAllocate
  --wrap=SymbianRuntimeReallocate
  --wrap=SymbianRuntimeFree
)
target_include_directories(ommesh_belle PRIVATE src)
# The HTTP SDK uses the streams libc++ ABI. Place its configuration ahead of
# the native-leave helper's default SDK include paths.
target_include_directories(ommesh_belle SYSTEM BEFORE PRIVATE
  "${SYMBIAN_SDK_PREFIX}/include/stream-config"
)
target_include_directories(ommesh_belle SYSTEM PRIVATE
  "$<TARGET_PROPERTY:MbedTLS::mbedtls,INTERFACE_INCLUDE_DIRECTORIES>"
)
set_property(SOURCE src/native_tile_https.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "$<TARGET_PROPERTY:MbedTLS::mbedtls,INTERFACE_COMPILE_DEFINITIONS>"
)
target_compile_options(ommesh_belle PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/src/qt4_iterators.hpp>"
)
target_link_libraries(ommesh_belle PRIVATE
  Symbian::Streams
  Symbian::QtCore
  Symbian::QtGui
  Symbian::QtNetwork
  Symbian::Http
  Symbian::Tls
  Symbian::Vibra
  "${SYMBIAN_SDK_PREFIX}/proxies/efsrv/efsrv.dso"
  "${SYMBIAN_SDK_PREFIX}/proxies/lbs/lbs.dso"
  "${SYMBIAN_SDK_PREFIX}/proxies/cone/cone.dso"
  "${SYMBIAN_SDK_PREFIX}/proxies/eikcore/eikcore.dso"
)

symbian_enable_native_leaves(ommesh_belle SOURCES src/main.cpp src/native_vibration.cpp src/native_location.cpp src/native_keyboard.cpp)
set_property(SOURCE src/main.cpp src/native_vibration.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-pic -fno-pie)

# Immutable PIC vtables contain loader imports. Put them in the code image
# alongside ordinary read-only data, leaving the application objects PIC.
function(ommesh_place_vtables_in_code)
  get_target_property(original ommesh_belle SYMBIAN_IMAGE_LAYOUT)
  file(READ "${original}" layout)
  string(REPLACE " .ARM.extab :"
    " .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :code\n .ARM.extab :"
    adjusted_layout "${layout}")
  if(layout STREQUAL adjusted_layout)
    message(FATAL_ERROR "The Symbian linker script lacks the expected unwind section.")
  endif()
  set(script "${CMAKE_CURRENT_BINARY_DIR}/ommesh_belle_image.ld")
  file(CONFIGURE OUTPUT "${script}" CONTENT "${adjusted_layout}" @ONLY)
  get_target_property(options ommesh_belle LINK_OPTIONS)
  list(FIND options "${original}" position)
  if(position LESS 0)
    message(FATAL_ERROR "Could not find the Symbian linker script in the link options.")
  endif()
  list(REMOVE_AT options ${position})
  list(INSERT options ${position} "${script}")
  set_target_properties(ommesh_belle PROPERTIES
    LINK_OPTIONS "${options}" SYMBIAN_IMAGE_LAYOUT "${script}")
  set_property(TARGET ommesh_belle APPEND PROPERTY LINK_DEPENDS "${script}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
endfunction()
cmake_language(DEFER CALL ommesh_place_vtables_in_code)

# E32 capability bits 13 (NetworkServices) and 17 (Location).
symbian_publish_executable(ommesh_belle UID3 0xe78af613 CAPABILITIES 0x22000)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(belle_executable "${CMAKE_CURRENT_BINARY_DIR}/belle/ommesh_belle.exe")
add_custom_command(
  OUTPUT "${belle_executable}"
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_belle_executable.py"
    "${CMAKE_CURRENT_BINARY_DIR}/e32/ommesh_belle.exe"
    "${belle_executable}"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/e32/ommesh_belle.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_belle_executable.py"
  VERBATIM
)
add_custom_target(ommesh_belle_compat DEPENDS "${belle_executable}")
