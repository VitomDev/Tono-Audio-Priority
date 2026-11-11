# Tono.exe, its core library and the tests. Use build.ps1, which also fetches
# the build tools and builds the installer.
cmake_minimum_required(VERSION 3.21)
project(Tono CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no Visual C++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /WX /permissive- /sdl /utf-8 /guard:cf /external:anglebrackets /external:W0)
add_link_options(/guard:cf /CETCOMPAT /OPT:REF /OPT:ICF)

add_library(tonocore STATIC
    src/core/audio.cpp
    src/core/headset.cpp
    src/core/paths.cpp
    src/core/priority.cpp
    src/core/settings.cpp
)
target_include_directories(tonocore PUBLIC src)
target_link_libraries(tonocore PUBLIC user32 advapi32 ole32 shell32 bcrypt urlmon propsys)

add_executable(Tono WIN32
    src/app/main.cpp
    src/app/panel.cpp
    src/app/theme.cpp
    src/app/tray_app.cpp
    src/app/Tono.rc
)
target_link_libraries(Tono PRIVATE tonocore gdi32 comctl32 dwmapi uxtheme)
# rc.exe does not track its includes, so the version number would stay stale.
set_source_files_properties(src/app/Tono.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/version.h;${CMAKE_SOURCE_DIR}/assets/tono.ico")
# The manifest is embedded through Tono.rc.
target_link_options(Tono PRIVATE /MANIFEST:NO)

add_executable(TonoTests tests/tests.cpp)
target_link_libraries(TonoTests PRIVATE tonocore)
