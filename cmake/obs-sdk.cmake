# Resolves OBS::libobs, OBS::obs-frontend-api and Qt6::Widgets.
#
# VMIX_USE_INSTALLED_OBS (Windows default): build against the OBS you actually run. On first configure this
#   - downloads the OBS sources of that release (only headers are extracted),
#   - downloads OBS' prebuilt Qt (only QtCore/QtGui/QtWidgets headers + import libs are extracted),
#   - generates import libraries from the installed obs.dll / obs-frontend-api.dll,
# so the binary matches the installed OBS exactly. Everything lands in .deps/ and is reused afterwards.
#
# Otherwise the regular CMake packages are used (full OBS build tree / obs-plugintemplate dependencies).

set(VMIX_OBS_VERSION 32.2.1)
set(VMIX_OBS_SHA256 db034bc3d2c137ec7f1809cbef4fc90c8948652b90a860078410482457d8a574)
set(VMIX_QT_DEPS_VERSION 2026-07-15) # obs-deps release used by OBS ${VMIX_OBS_VERSION} (Qt 6.11.1)
set(VMIX_QT_SHA256 7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579)

if(WIN32)
  set(_vmix_default_installed ON)
else()
  set(_vmix_default_installed OFF)
endif()
option(VMIX_USE_INSTALLED_OBS "Build against the installed OBS instead of CMake packages" ${_vmix_default_installed})

if(NOT VMIX_USE_INSTALLED_OBS)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
  set_property(TARGET Qt6::Core APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS QT_NO_KEYWORDS)
  return()
endif()

# --- locate the installed OBS ---------------------------------------------------------------------------

if(NOT OBS_INSTALL_DIR)
  cmake_host_system_information(
    RESULT _vmix_reg
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/OBS Studio"
    ERROR_VARIABLE _vmix_reg_err
  )
  foreach(_dir IN ITEMS "${_vmix_reg}" "$ENV{ProgramFiles}/obs-studio")
    if(_dir AND EXISTS "${_dir}/bin/64bit/obs.dll")
      file(TO_CMAKE_PATH "${_dir}" _vmix_found)
      break()
    endif()
  endforeach()
  set(OBS_INSTALL_DIR "${_vmix_found}" CACHE PATH "OBS Studio installation directory")
endif()
if(NOT EXISTS "${OBS_INSTALL_DIR}/bin/64bit/obs.dll")
  message(FATAL_ERROR "OBS Studio not found. Configure with -DOBS_INSTALL_DIR=<path to obs-studio>.")
endif()

set(_deps "${CMAKE_SOURCE_DIR}/.deps")
set(OBS_SOURCE_DIR "${_deps}/obs-studio-${VMIX_OBS_VERSION}")
set(OBS_QT_DIR "${_deps}/qt6")
set(OBS_SDK_DIR "${_deps}/sdk")

# --- OBS headers ------------------------------------------------------------------------------------------

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
  set(_tar "${_deps}/obs-studio-${VMIX_OBS_VERSION}.tar.gz")
  message(STATUS "Downloading OBS ${VMIX_OBS_VERSION} sources (headers only are kept)")
  file(
    DOWNLOAD "https://github.com/obsproject/obs-studio/archive/refs/tags/${VMIX_OBS_VERSION}.tar.gz" "${_tar}"
    EXPECTED_HASH SHA256=${VMIX_OBS_SHA256}
  )
  set(_root "obs-studio-${VMIX_OBS_VERSION}")
  file(
    ARCHIVE_EXTRACT
    INPUT "${_tar}"
    DESTINATION "${_deps}"
    PATTERNS "${_root}/libobs/*" "${_root}/frontend/api/*" "${_root}/deps/w32-pthreads/*"
  )
  file(REMOVE "${_tar}")
endif()

if(NOT EXISTS "${OBS_SDK_DIR}/include/obsconfig.h")
  file(WRITE "${OBS_SDK_DIR}/include/obsconfig.h" "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
endif()

# --- Qt (the exact build OBS ships) -----------------------------------------------------------------------

if(NOT EXISTS "${OBS_QT_DIR}/lib/Qt6Widgets.lib")
  set(_zip "${_deps}/qt6-${VMIX_QT_DEPS_VERSION}.zip")
  message(STATUS "Downloading OBS Qt6 build ${VMIX_QT_DEPS_VERSION} (~250 MB, ~20 MB kept)")
  file(
    DOWNLOAD
      "https://github.com/obsproject/obs-deps/releases/download/${VMIX_QT_DEPS_VERSION}/windows-deps-qt6-${VMIX_QT_DEPS_VERSION}-x64.zip"
      "${_zip}"
    EXPECTED_HASH SHA256=${VMIX_QT_SHA256}
  )
  file(
    ARCHIVE_EXTRACT
    INPUT "${_zip}"
    DESTINATION "${OBS_QT_DIR}"
    PATTERNS
      "include/QtCore/*"
      "include/QtGui/*"
      "include/QtWidgets/*"
      "lib/Qt6Core.lib"
      "lib/Qt6Gui.lib"
      "lib/Qt6Widgets.lib"
  )
  file(GLOB _private LIST_DIRECTORIES true "${OBS_QT_DIR}/include/Qt*/6.*")
  if(_private)
    file(REMOVE_RECURSE ${_private})
  endif()
  file(REMOVE "${_zip}")
endif()

# --- import libraries from the installed DLLs -------------------------------------------------------------

if(NOT MSVC)
  message(FATAL_ERROR "VMIX_USE_INSTALLED_OBS requires MSVC (run from an x64 Visual Studio developer prompt).")
endif()
get_filename_component(_tooldir "${CMAKE_LINKER}" DIRECTORY)
find_program(VMIX_DUMPBIN dumpbin HINTS "${_tooldir}" REQUIRED)

function(vmix_import_lib dll name)
  set(lib "${OBS_SDK_DIR}/lib/${name}.lib")
  if(EXISTS "${lib}" AND NOT "${dll}" IS_NEWER_THAN "${lib}")
    return()
  endif()
  execute_process(
    COMMAND "${VMIX_DUMPBIN}" /nologo /exports "${dll}"
    OUTPUT_VARIABLE out
    COMMAND_ERROR_IS_FATAL ANY
  )
  string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ +[0-9A-F]+ +[^ \r\n]+" rows "${out}")
  set(def "LIBRARY ${name}\nEXPORTS\n")
  foreach(row IN LISTS rows)
    string(REGEX REPLACE ".* ([^ ]+)$" "\\1" symbol "${row}")
    string(APPEND def "  ${symbol}\n")
  endforeach()
  list(LENGTH rows count)
  if(count EQUAL 0)
    message(FATAL_ERROR "No exports found in ${dll}")
  endif()
  file(WRITE "${OBS_SDK_DIR}/lib/${name}.def" "${def}")
  execute_process(
    COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${OBS_SDK_DIR}/lib/${name}.def" "/out:${lib}"
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY
  )
  message(STATUS "Generated ${name}.lib (${count} exports)")
endfunction()

vmix_import_lib("${OBS_INSTALL_DIR}/bin/64bit/obs.dll" obs)
vmix_import_lib("${OBS_INSTALL_DIR}/bin/64bit/obs-frontend-api.dll" obs-frontend-api)

# --- imported targets -------------------------------------------------------------------------------------

add_library(OBS::libobs SHARED IMPORTED)
set_target_properties(
  OBS::libobs
  PROPERTIES
    IMPORTED_IMPLIB "${OBS_SDK_DIR}/lib/obs.lib"
    IMPORTED_LOCATION "${OBS_INSTALL_DIR}/bin/64bit/obs.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${OBS_SDK_DIR}/include;${OBS_SOURCE_DIR}/deps/w32-pthreads"
)

add_library(OBS::obs-frontend-api SHARED IMPORTED)
set_target_properties(
  OBS::obs-frontend-api
  PROPERTIES
    IMPORTED_IMPLIB "${OBS_SDK_DIR}/lib/obs-frontend-api.lib"
    IMPORTED_LOCATION "${OBS_INSTALL_DIR}/bin/64bit/obs-frontend-api.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/frontend/api"
    INTERFACE_LINK_LIBRARIES OBS::libobs
)

foreach(_mod Core Gui Widgets)
  add_library(Qt6::${_mod} SHARED IMPORTED)
  set_target_properties(
    Qt6::${_mod}
    PROPERTIES
      IMPORTED_IMPLIB "${OBS_QT_DIR}/lib/Qt6${_mod}.lib"
      IMPORTED_LOCATION "${OBS_INSTALL_DIR}/bin/64bit/Qt6${_mod}.dll"
      INTERFACE_INCLUDE_DIRECTORIES "${OBS_QT_DIR}/include;${OBS_QT_DIR}/include/Qt${_mod}"
  )
endforeach()
set_property(TARGET Qt6::Gui PROPERTY INTERFACE_LINK_LIBRARIES Qt6::Core)
set_property(TARGET Qt6::Widgets PROPERTY INTERFACE_LINK_LIBRARIES Qt6::Gui)
set_property(TARGET Qt6::Core PROPERTY INTERFACE_COMPILE_DEFINITIONS "QT_NO_DEBUG;QT_NO_KEYWORDS")
