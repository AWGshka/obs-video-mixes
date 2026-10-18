cmake_minimum_required(VERSION 3.28)

project(obs-video-mixes VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# `cmake --install build` drops the plugin straight into OBS' per-machine plugin folder.
if(WIN32 AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  file(TO_CMAKE_PATH "$ENV{ProgramData}/obs-studio/plugins" _vmix_prefix)
  set(CMAKE_INSTALL_PREFIX "${_vmix_prefix}" CACHE PATH "Install prefix" FORCE)
endif()

option(VMIX_BUILD_SMOKE_TEST "Build the headless libobs smoke test" ON)

include(cmake/obs-sdk.cmake)

add_library(${PROJECT_NAME} MODULE)

target_sources(
  ${PROJECT_NAME}
  PRIVATE
    src/plugin.cpp
    src/core/config.cpp
    src/core/encoders.cpp
    src/core/frame-rate.cpp
    src/core/item-path.cpp
    src/core/manager.cpp
    src/core/mix-canvas.cpp
    src/core/obs-recording.cpp
    src/core/recording.cpp
    src/core/visibility.cpp
    src/render/geometry.cpp
    src/render/mix-renderer.cpp
    src/render/root-source.cpp
    src/ui/dock.cpp
    src/ui/mix-settings-dialog.cpp
    src/ui/preview.cpp
    src/ui/properties-form.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_definitions(${PROJECT_NAME} PRIVATE VMIX_VERSION="${PROJECT_VERSION}")
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)

if(MSVC)
  target_compile_options(
    ${PROJECT_NAME}
    PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /wd4100 /wd4505
  )
  # _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR: stay compatible with older msvcp140.dll that OBS may load first.
  target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
  )
  target_link_options(${PROJECT_NAME} PRIVATE /OPT:REF /OPT:ICF)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# OBS 30+ plugin layout: <plugin>/bin/64bit/<plugin>.dll + <plugin>/data/
install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${PROJECT_NAME}/bin/64bit)
if(MSVC)
  install(FILES $<TARGET_PDB_FILE:${PROJECT_NAME}> DESTINATION ${PROJECT_NAME}/bin/64bit OPTIONAL)
endif()
install(DIRECTORY data/ DESTINATION ${PROJECT_NAME}/data)

# `cmake --build build --target package` -> zip that extracts into %ProgramData%/obs-studio/plugins
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)

if(VMIX_BUILD_SMOKE_TEST)
  add_executable(vmix-smoke tests/smoke.cpp src/core/encoders.cpp src/core/frame-rate.cpp src/core/item-path.cpp)
  target_include_directories(vmix-smoke PRIVATE src)
  target_link_libraries(vmix-smoke PRIVATE OBS::libobs OBS::obs-frontend-api)
  target_compile_definitions(vmix-smoke PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(vmix-smoke PRIVATE /W4 /permissive- /utf-8 /wd4100)
  endif()

  if(OBS_INSTALL_DIR)
    # OBS plugins start helper processes (encoder probes, muxer) from the host executable's directory.
    foreach(_helper obs-nvenc-test obs-qsv-test obs-amf-test obs-ffmpeg-mux)
      if(EXISTS "${OBS_INSTALL_DIR}/bin/64bit/${_helper}.exe")
        add_custom_command(
          TARGET vmix-smoke
          POST_BUILD
          COMMAND
            ${CMAKE_COMMAND} -E copy_if_different "${OBS_INSTALL_DIR}/bin/64bit/${_helper}.exe"
            $<TARGET_FILE_DIR:vmix-smoke>
        )
      endif()
    endforeach()

    enable_testing()
    # Headless libobs + D3D11 render check against the installed OBS; does not touch OBS settings.
    add_test(
      NAME smoke
      COMMAND vmix-smoke "${OBS_INSTALL_DIR}" $<TARGET_FILE:${PROJECT_NAME}> "${CMAKE_SOURCE_DIR}/data"
      WORKING_DIRECTORY "${OBS_INSTALL_DIR}/bin/64bit"
    )
    set_tests_properties(
      smoke
      PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_INSTALL_DIR}/bin/64bit" TIMEOUT 60
    )
  endif()
endif()
