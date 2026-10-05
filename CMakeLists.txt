cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# The public "Gleem for OBS" app on gleem.gg (device flow, irl:read). Builds for
# a local gleem.gg pass their own; GLEEM_OAUTH_CLIENT_ID in the environment
# overrides it at runtime too.
set(GLEEM_OAUTH_CLIENT_ID "gla_ze30f3k4" CACHE STRING "OAuth client id of the Gleem for OBS app")
set(GLEEM_API_URL "https://gleem.gg" CACHE STRING "Default gleem.gg base URL")

# Install into the per-user plugin directory instead of the system one. Inside
# the OBS Flatpak sandbox XDG_CONFIG_HOME points at
# ~/.var/app/com.obsproject.Studio/config, which is where Flatpak OBS looks.
option(LINUX_PORTABLE "Install into the per-user OBS plugin directory (Linux)" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(CURL REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets CURL::libcurl
)

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON CXX_STANDARD 17
)

target_compile_definitions(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    GLEEM_OAUTH_CLIENT_ID="${GLEEM_OAUTH_CLIENT_ID}"
    GLEEM_DEFAULT_API_URL="${GLEEM_API_URL}"
    GLEEM_PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}"
    QT_NO_KEYWORDS
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/gleem-http.cpp
    src/gleem-http.hpp
    src/gleem-account.cpp
    src/gleem-account.hpp
    src/sign-in-dialog.cpp
    src/sign-in-dialog.hpp
    src/sidekick-source.cpp
    src/sidekick-source.hpp
    src/log-redact.cpp
    src/log-redact.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(LINUX_PORTABLE AND OS_LINUX)
  if(DEFINED ENV{XDG_CONFIG_HOME} AND NOT "$ENV{XDG_CONFIG_HOME}" STREQUAL "")
    set(_config_home "$ENV{XDG_CONFIG_HOME}")
  else()
    set(_config_home "$ENV{HOME}/.config")
  endif()
  set(_portable "${_config_home}/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
  install(
    TARGETS ${CMAKE_PROJECT_NAME}
    LIBRARY DESTINATION "${_portable}/bin/64bit" COMPONENT portable EXCLUDE_FROM_ALL
  )
  install(DIRECTORY data/ DESTINATION "${_portable}/data" COMPONENT portable EXCLUDE_FROM_ALL)
endif()
