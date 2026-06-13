cmake_minimum_required(VERSION 3.22)

project(portail_capture_plugin LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "Portail Capture native plugin currently supports Windows only.")
endif()

set(UNITY_NATIVE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../unitywebrtc_steamworks/Plugin~/unity/include")
if(NOT EXISTS "${UNITY_NATIVE_INCLUDE_DIR}/IUnityInterface.h")
  set(UNITY_NATIVE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/src/plugin")
  if(NOT EXISTS "${UNITY_NATIVE_INCLUDE_DIR}/IUnityInterface.h")
    message(FATAL_ERROR "Unity native plugin headers not found. Checked external path and ${UNITY_NATIVE_INCLUDE_DIR}.")
  endif()
  message(WARNING "Unity external plugin headers not found, falling back to local headers in ${UNITY_NATIVE_INCLUDE_DIR}.")
endif()

add_library(portail_capture_plugin SHARED
  src/plugin/virtualdesktop_plugin.cpp
  src/common/virtualdesktop_audio_stream.cpp
  src/common/audio_stream.h
  src/common/time_utils.h
)

target_include_directories(portail_capture_plugin
  PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${UNITY_NATIVE_INCLUDE_DIR}"
)

target_compile_definitions(portail_capture_plugin
  PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(portail_capture_plugin PRIVATE /W4 /EHsc /permissive-)

target_link_libraries(portail_capture_plugin
  PRIVATE
    user32
    gdi32
    ole32
    mmdevapi
    uuid
    avrt
    winmm
    d3d11
    dxgi
    runtimeobject
    windowsapp
)

get_filename_component(PORTAIL_CAPTURE_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
set(PORTAIL_CAPTURE_UNITY_PLUGIN_OUTPUT_DIR "${PORTAIL_CAPTURE_ROOT}/Plugins")

function(copy_portail_capture_to_unity target_name)
  add_custom_command(
    TARGET "${target_name}" POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PORTAIL_CAPTURE_UNITY_PLUGIN_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:${target_name}>"
      "${PORTAIL_CAPTURE_UNITY_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:${target_name}>"
    VERBATIM
  )
endfunction()

copy_portail_capture_to_unity(portail_capture_plugin)
