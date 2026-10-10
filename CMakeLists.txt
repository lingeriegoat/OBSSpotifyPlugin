set(PLUGIN_AUTHOR "lingerie goat")

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs gdiplus shlwapi)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/spotify-source.cpp src/spotify-source.h )

if(MSVC)
  set_source_files_properties(src/spotify-source.cpp PROPERTIES COMPILE_OPTIONS /EHa)
endif()

set(_obs_layout_dir "${CMAKE_BINARY_DIR}/Plugin_Release")
set(_zip_base "OBSSpotifyNativePlugin${PLUGIN_VERSION_RAW}")
set(_zip_modern "${_zip_base}_modern.zip")
set(_zip_legacy "${_zip_base}_legacy.zip")

set(_stage_modern "${_obs_layout_dir}/_stage_modern")
set(_stage_legacy "${_obs_layout_dir}/_stage_legacy")
set(_data_src "${CMAKE_CURRENT_SOURCE_DIR}/data/obs-plugins/${_name}")

add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_obs_layout_dir}"

  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_stage_modern}/${_name}/data"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
          "${_stage_modern}/${_name}/${_name}.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${_data_src}"
          "${_stage_modern}/${_name}/data"
  COMMAND ${CMAKE_COMMAND} -E chdir "${_stage_modern}"
          ${CMAKE_COMMAND} -E tar cf "../${_zip_modern}" --format=zip -- "${_name}"

  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_stage_legacy}/${_name}/bin/64bit"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_stage_legacy}/${_name}/data"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
          "${_stage_legacy}/${_name}/bin/64bit/${_name}.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${_data_src}"
          "${_stage_legacy}/${_name}/data"
  COMMAND ${CMAKE_COMMAND} -E chdir "${_stage_legacy}"
          ${CMAKE_COMMAND} -E tar cf "../${_zip_legacy}" --format=zip -- "${_name}"

  COMMENT "Assembling modern and legacy OBS plugin packages in ${_obs_layout_dir}"
  VERBATIM
)