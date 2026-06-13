cmake_minimum_required(VERSION 3.22)

project(portail_stream_plugin LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "This prototype currently supports Windows only.")
endif()

set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/Reference Code/steamworks_sdk" CACHE PATH "Steamworks SDK root")
foreach(_steamworks_candidate IN ITEMS
  "${CMAKE_SOURCE_DIR}/../../../SteamStream Plugin Reference Code/steamworks_sdk"
  "${CMAKE_SOURCE_DIR}/../../../../SteamStream Plugin Reference Code/steamworks_sdk"
  "${CMAKE_SOURCE_DIR}/../../../../../SteamStream Plugin Reference Code/steamworks_sdk"
)
  if(NOT EXISTS "${STEAMWORKS_SDK_DIR}" AND EXISTS "${_steamworks_candidate}")
    set(STEAMWORKS_SDK_DIR "${_steamworks_candidate}" CACHE PATH "Steamworks SDK root" FORCE)
    break()
  endif()
endforeach()
set(STEAMWORKS_SDK_BASE "${STEAMWORKS_SDK_DIR}")
if(EXISTS "${STEAMWORKS_SDK_DIR}/sdk/public" AND NOT EXISTS "${STEAMWORKS_SDK_DIR}/public")
  set(STEAMWORKS_SDK_BASE "${STEAMWORKS_SDK_DIR}/sdk")
endif()
set(FFMPEG_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/Reference Code/ffmpeg-master-latest-win64-gpl-shared")
foreach(_ffmpeg_candidate IN ITEMS
  "${CMAKE_SOURCE_DIR}/../../../SteamStream Plugin Reference Code/ffmpeg-master-latest-win64-gpl-shared"
  "${CMAKE_SOURCE_DIR}/../../../../SteamStream Plugin Reference Code/ffmpeg-master-latest-win64-gpl-shared"
  "${CMAKE_SOURCE_DIR}/../../../../../SteamStream Plugin Reference Code/ffmpeg-master-latest-win64-gpl-shared"
  "${CMAKE_SOURCE_DIR}/Reference Code/AMF-1.5.0/Thirdparty/ffmpeg/ffmpeg/ffmpeg-7.0/x64/release"
  "${CMAKE_SOURCE_DIR}/../../../SteamStream Plugin Reference Code/AMF-1.5.0/Thirdparty/ffmpeg/ffmpeg/ffmpeg-7.0/x64/release"
  "${CMAKE_SOURCE_DIR}/../../../../SteamStream Plugin Reference Code/AMF-1.5.0/Thirdparty/ffmpeg/ffmpeg/ffmpeg-7.0/x64/release"
  "${CMAKE_SOURCE_DIR}/../../../../../SteamStream Plugin Reference Code/AMF-1.5.0/Thirdparty/ffmpeg/ffmpeg/ffmpeg-7.0/x64/release"
)
  if(NOT EXISTS "${FFMPEG_ROOT_DEFAULT}/include" AND EXISTS "${_ffmpeg_candidate}/include")
    set(FFMPEG_ROOT_DEFAULT "${_ffmpeg_candidate}")
    break()
  endif()
endforeach()
set(FFMPEG_ROOT "${FFMPEG_ROOT_DEFAULT}" CACHE PATH "FFmpeg prebuilt root")

set(STEAMWORKS_INCLUDE_DIR "${STEAMWORKS_SDK_BASE}/public")
set(STEAMWORKS_LIB "${STEAMWORKS_SDK_BASE}/redistributable_bin/win64/steam_api64.lib")
set(EXTERNAL_DLL_DIR "${CMAKE_SOURCE_DIR}/external DLLs")
set(STEAMWORKS_DLL "${EXTERNAL_DLL_DIR}/steam_api64.dll")
set(STEAMWEBRTC_DLL "")
foreach(_steamwebrtc_candidate IN ITEMS
  "${EXTERNAL_DLL_DIR}/steamwebrtc64.dll"
  "${STEAMWORKS_SDK_BASE}/redistributable_bin/win64/steamwebrtc64.dll"
  "C:/Program Files (x86)/Steam/steamwebrtc64.dll"
  "$ENV{ProgramFiles}/Steam/steamwebrtc64.dll"
  "C:/Program Files/Steam/steamwebrtc64.dll"
)
  if(EXISTS "${_steamwebrtc_candidate}")
    set(STEAMWEBRTC_DLL "${_steamwebrtc_candidate}")
    break()
  endif()
endforeach()

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")
set(FFMPEG_BIN_DIR "${EXTERNAL_DLL_DIR}")
file(GLOB FFMPEG_RUNTIME_DLLS
  "${FFMPEG_BIN_DIR}/av*.dll"
  "${FFMPEG_BIN_DIR}/sw*.dll"
)

foreach(path IN ITEMS
  "${STEAMWORKS_INCLUDE_DIR}"
  "${STEAMWORKS_LIB}"
  "${STEAMWORKS_DLL}"
  "${FFMPEG_INCLUDE_DIR}"
  "${FFMPEG_LIB_DIR}/avcodec.lib"
  "${FFMPEG_LIB_DIR}/avutil.lib"
  "${FFMPEG_LIB_DIR}/swresample.lib"
  "${FFMPEG_LIB_DIR}/swscale.lib"
)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Required dependency path is missing: ${path}")
  endif()
endforeach()
if(NOT FFMPEG_RUNTIME_DLLS)
  message(FATAL_ERROR "No FFmpeg runtime DLLs found in ${FFMPEG_BIN_DIR}")
endif()
if(NOT STEAMWEBRTC_DLL)
  message(WARNING "steamwebrtc64.dll not found. Direct ICE will fail with 'Failed to load steamwebrtc64.dll'.")
else()
  message(STATUS "Using steamwebrtc64.dll: ${STEAMWEBRTC_DLL}")
endif()

set(STREAM_SDR_SOURCES
  src/common/steam_sdr.cpp
)

set(STREAM_AUDIO_SOURCES
  src/common/audio_stream.cpp
)

set(COMMON_HEADERS
  src/common/audio_stream.h
  src/common/codec_utils.h
  src/common/connection_path.h
  src/common/d3d11_unity_utils.h
  src/common/lod_utils.h
  src/common/plugin_log.h
  src/common/protocol.h
  src/common/protocol_validation.h
  src/common/steam_sdr.h
  src/common/time_utils.h
  src/common/unity_texture_copy.h
  src/plugin/receiver_plugin_api.h
  src/plugin/sender_plugin_api.h
  src/receiver/receiver_audio_pipeline.h
  src/receiver/receiver_connection_state.h
  src/receiver/receiver_peer_state.h
  src/receiver/receiver_protocol_sender.h
  src/receiver/receiver_quality_state.h
  src/receiver/receiver_stats.h
  src/receiver/ffmpeg_decoder.h
  src/receiver/frame_assembler.h
  src/receiver/gpu_texture_bridge.h
  src/receiver/recovery_unit.h
  src/receiver/receiver_video_decode_queue.h
  src/receiver/receiver_video_state.h
  src/sender/encoded_packet.h
  src/sender/ffmpeg_encoder.h
  src/sender/sender_lod_config.h
  src/sender/sender_media_sender.h
  src/sender/sender_peer_sender.h
  src/sender/sender_options.h
  src/sender/sender_route_state.h
  src/sender/sender_runtime_state.h
  src/sender/sender_stats.h
  src/sender/sdr_queue_status.h
  src/sender/shared_texture_mirror.h
  src/sender/virtual_desktop_capture_bridge.h
)

set(UNITY_NATIVE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../unitywebrtc_steamworks/Plugin~/unity/include")
if(NOT EXISTS "${UNITY_NATIVE_INCLUDE_DIR}/IUnityInterface.h")
  set(UNITY_NATIVE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/src/plugin")
  if(NOT EXISTS "${UNITY_NATIVE_INCLUDE_DIR}/IUnityInterface.h")
    message(FATAL_ERROR "Unity native plugin headers not found. Checked external path and ${UNITY_NATIVE_INCLUDE_DIR}.")
  endif()
  message(WARNING "Unity external plugin headers not found, falling back to local headers in ${UNITY_NATIVE_INCLUDE_DIR}.")
endif()

add_library(portail_stream_common SHARED ${STREAM_SDR_SOURCES} ${COMMON_HEADERS})
target_include_directories(portail_stream_common
  PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${STEAMWORKS_INCLUDE_DIR}"
    "${FFMPEG_INCLUDE_DIR}"
)
target_compile_definitions(portail_stream_common
  PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    STREAM_COMMON_SHARED
  PRIVATE
    STREAM_COMMON_BUILD
)
target_compile_options(portail_stream_common PRIVATE /W4 /EHsc /permissive-)
target_link_libraries(portail_stream_common
  PUBLIC
    "${STEAMWORKS_LIB}"
    ws2_32
    user32
    gdi32
    uuid
    d3d11
    dxgi
    runtimeobject
)
add_library(stream_audio STATIC ${STREAM_AUDIO_SOURCES} ${COMMON_HEADERS})
target_include_directories(stream_audio
  PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${FFMPEG_INCLUDE_DIR}"
)
target_compile_definitions(stream_audio
  PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(stream_audio PRIVATE /W4 /EHsc /permissive-)
target_link_libraries(stream_audio
  PUBLIC
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    "${FFMPEG_LIB_DIR}/swresample.lib"
    "${FFMPEG_LIB_DIR}/swscale.lib"
)
add_library(portail_stream_sender_plugin SHARED src/plugin/sender_plugin.cpp)
target_link_libraries(portail_stream_sender_plugin PRIVATE portail_stream_common stream_audio)
target_include_directories(portail_stream_sender_plugin PRIVATE "${UNITY_NATIVE_INCLUDE_DIR}")
target_compile_definitions(portail_stream_sender_plugin PRIVATE PORTAIL_STREAM_PLUGIN_BUILD)
target_compile_options(portail_stream_sender_plugin PRIVATE /W4 /EHsc /permissive-)

add_library(portail_stream_receiver_plugin SHARED src/plugin/receiver_plugin.cpp)
target_link_libraries(portail_stream_receiver_plugin PRIVATE portail_stream_common stream_audio)
target_include_directories(portail_stream_receiver_plugin PRIVATE "${UNITY_NATIVE_INCLUDE_DIR}")
target_compile_options(portail_stream_receiver_plugin PRIVATE /W4 /EHsc /permissive-)

option(PORTAIL_STREAM_COPY_TO_UNITY "Copy built Portail Stream DLLs into the Unity Assets plugin folders" ON)

get_filename_component(PORTAIL_STREAM_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
set(UNITY_PLUGIN_OUTPUT_DIR "${PORTAIL_STREAM_ROOT}/Plugins")

if(PORTAIL_STREAM_COPY_TO_UNITY)
  add_custom_target(copy_portail_stream_plugins ALL
    DEPENDS
      portail_stream_sender_plugin
      portail_stream_receiver_plugin
      portail_stream_common
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UNITY_PLUGIN_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:portail_stream_sender_plugin>"
      "${UNITY_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:portail_stream_sender_plugin>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:portail_stream_receiver_plugin>"
      "${UNITY_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:portail_stream_receiver_plugin>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:portail_stream_common>"
      "${UNITY_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:portail_stream_common>"
    VERBATIM
  )

  if(STEAMWEBRTC_DLL)
    add_custom_command(
      TARGET copy_portail_stream_plugins POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${STEAMWEBRTC_DLL}"
        "${UNITY_PLUGIN_OUTPUT_DIR}/steamwebrtc64.dll"
      VERBATIM
    )
  endif()
  foreach(dll IN LISTS FFMPEG_RUNTIME_DLLS)
    add_custom_command(
      TARGET copy_portail_stream_plugins POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${dll}"
        "${UNITY_PLUGIN_OUTPUT_DIR}"
      VERBATIM
    )
  endforeach()
endif()



