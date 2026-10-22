# ZyDeck Connect: the computer side of ZyDeck's controller mode (see src/main.cpp). Qt 6 and C++17; RtMidi
# (third_party/rtmidi, MIT-style) for MIDI, Project Nayuki's qrcodegen (third_party/qrcodegen, MIT) for the
# pairing code. The ZyDeck mapping comes from ../../res/controllers, built in.
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt> && cmake --build build
cmake_minimum_required(VERSION 3.21)
project(ZyDeckConnect VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ZC_OFFSCREEN (a static Qt on a machine without X11's headers, e.g. the build PC): only Qt's offscreen platform,
# to run it there without a display and draw the window to a picture
option(ZC_OFFSCREEN "Static Qt: link only the offscreen platform plugin" OFF)
if(ZC_OFFSCREEN)
  set(QT_SKIP_AUTO_PLUGIN_INCLUSION ON)
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Sql)

# The mapping (the phone's own, named for the desktop when it's installed)
set(MAPPING_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../res/controllers")
foreach(f Zydek-Tablet.midi.xml Zydek-Tablet.script.js)
  configure_file("${MAPPING_SRC}/${f}" "${CMAKE_CURRENT_BINARY_DIR}/mapping/${f}" COPYONLY)
endforeach()

qt_add_executable(zydeck-connect WIN32 MACOSX_BUNDLE
  src/main.cpp
  src/core.cpp src/core.h
  src/library.cpp src/library.h
  src/midi.cpp src/midi.h
  src/system.cpp src/system.h
  src/setup.cpp src/setup.h
  src/discovery.cpp src/discovery.h
  src/window.cpp src/window.h
  third_party/rtmidi/RtMidi.cpp third_party/rtmidi/RtMidi.h
  third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp
)
qt_add_resources(zydeck-connect mapping
  PREFIX /mapping
  BASE "${CMAKE_CURRENT_BINARY_DIR}/mapping"
  FILES "${CMAKE_CURRENT_BINARY_DIR}/mapping/Zydek-Tablet.midi.xml" "${CMAKE_CURRENT_BINARY_DIR}/mapping/Zydek-Tablet.script.js"
)
target_include_directories(zydeck-connect PRIVATE third_party/rtmidi third_party/qrcodegen)
target_link_libraries(zydeck-connect PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Sql)

# RtMidi's backend
if(APPLE)
  target_compile_definitions(zydeck-connect PRIVATE __MACOSX_CORE__)
  target_link_libraries(zydeck-connect PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
  set_target_properties(zydeck-connect PROPERTIES
    OUTPUT_NAME "ZyDeck Connect"
    MACOSX_BUNDLE_BUNDLE_NAME "ZyDeck Connect"
    MACOSX_BUNDLE_GUI_IDENTIFIER org.zydeck.connect
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
elseif(WIN32)
  target_compile_definitions(zydeck-connect PRIVATE __WINDOWS_MM__)
  target_link_libraries(zydeck-connect PRIVATE winmm)
  set_target_properties(zydeck-connect PROPERTIES OUTPUT_NAME "ZyDeck Connect")
else()
  find_package(ALSA REQUIRED)
  find_package(Threads REQUIRED)
  target_compile_definitions(zydeck-connect PRIVATE __LINUX_ALSA__)
  target_link_libraries(zydeck-connect PRIVATE ALSA::ALSA Threads::Threads)
endif()

# A static Qt (a self-contained build) needs its plugins linked in: SQLite for Mixxx's library
get_target_property(QT_CORE_TYPE Qt6::Core TYPE)
if(QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
  set(ZC_PLUGINS QSQLiteDriverPlugin)
  if(ZC_OFFSCREEN)
    find_package(Qt6QOffscreenIntegrationPlugin CONFIG REQUIRED PATHS "${Qt6Gui_DIR}" NO_DEFAULT_PATH)
    find_package(Qt6QSQLiteDriverPlugin CONFIG REQUIRED PATHS "${Qt6Sql_DIR}" NO_DEFAULT_PATH)
    list(APPEND ZC_PLUGINS QOffscreenIntegrationPlugin)
  endif()
  foreach(p ${ZC_PLUGINS})   # the plugin, and its _init object that registers it (Q_IMPORT_PLUGIN)
    target_link_libraries(zydeck-connect PRIVATE Qt6::${p})
    if(TARGET Qt6::${p}_init)
      target_link_libraries(zydeck-connect PRIVATE Qt6::${p}_init)
    endif()
  endforeach()
endif()

install(TARGETS zydeck-connect BUNDLE DESTINATION . RUNTIME DESTINATION bin)
