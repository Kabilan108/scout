cmake_minimum_required(VERSION 3.24)
project(scout VERSION 0.3.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SCOUT_PLUGIN_DIRS "" CACHE STRING "Extra Qt plugin directories baked into the binary (semicolon separated)")
set(SCOUT_COMMIT "" CACHE STRING "Git commit shown by --version; read from git at configure time when empty")
option(SCOUT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(SCOUT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

if(NOT SCOUT_COMMIT)
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=7 --exclude=*
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE SCOUT_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  set(SCOUT_COMMIT "${SCOUT_GIT_COMMIT}")
  if(NOT SCOUT_COMMIT)
    set(SCOUT_COMMIT "unknown")
  endif()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets DBus Concurrent Test)
find_package(KF6SyntaxHighlighting REQUIRED)
find_package(LibArchive REQUIRED)

add_library(scoutcore STATIC
  src/theme.cpp
  src/fuzzy.cpp
  src/keymap.cpp
  src/dirmodel.cpp
  src/columnview.cpp
  src/preview.cpp
  src/fileops.cpp
  src/clipboard.cpp
  src/desktopentry.cpp
  src/finder.cpp
  src/prompt.cpp
  src/overlays.cpp
  src/commands.cpp
  src/chooser.cpp
  src/mainwindow.cpp
  src/service.cpp
  src/launcher.cpp
  src/logging.cpp
  src/buildinfo.cpp
  src/state.cpp
  src/startup.cpp
  src/config.cpp
  src/report.cpp
)
set_source_files_properties(src/buildinfo.cpp PROPERTIES COMPILE_DEFINITIONS "SCOUT_COMMIT=\"${SCOUT_COMMIT}\"")
target_include_directories(scoutcore PUBLIC src)
target_link_libraries(scoutcore PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus Qt6::Concurrent
  KF6::SyntaxHighlighting
  LibArchive::LibArchive
)
string(REPLACE ";" ":" SCOUT_PLUGIN_DIRS_JOINED "${SCOUT_PLUGIN_DIRS}")
target_compile_definitions(scoutcore PUBLIC
  SCOUT_VERSION="${PROJECT_VERSION}"
  SCOUT_PLUGIN_DIRS="${SCOUT_PLUGIN_DIRS_JOINED}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(scoutcore PRIVATE -Wall -Wextra)
endif()

add_executable(scout src/main.cpp)
target_link_libraries(scout PRIVATE scoutcore)
# Exports the binary's symbols so crash backtraces in scout.log have function names.
set_target_properties(scout PROPERTIES ENABLE_EXPORTS ON)

include(GNUInstallDirs)
install(TARGETS scout RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
foreach(service org.freedesktop.FileManager1 org.freedesktop.impl.portal.desktop.scout)
  configure_file(data/${service}.service.in ${CMAKE_CURRENT_BINARY_DIR}/${service}.service @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${service}.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
endforeach()
install(FILES data/scout.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES data/scout.desktop DESTINATION share/applications)
install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)

enable_testing()
add_executable(scout_tests tests/test_scout.cpp)
target_link_libraries(scout_tests PRIVATE scoutcore Qt6::Test)
add_test(NAME scout_tests COMMAND scout_tests)
