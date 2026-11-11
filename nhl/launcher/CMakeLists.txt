# NHLLauncher: setup + front door for the native NHL Legacy port.
# Build: tools\devenv.cmd cmake -S nhl\launcher -B nhl\build_launcher -G Ninja
#          -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
#        tools\devenv.cmd cmake --build nhl\build_launcher
cmake_minimum_required(VERSION 3.20)
project(NHLLauncher C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: one exe, no redist

set(IMGUI third_party/imgui)
add_executable(NHLLauncher WIN32
    main.cpp
    ui.cpp
    audio_pack.cpp
    launcher.rc
    bug_report.cpp
    installer.cpp
    mod_import.cpp
    iso9660.cpp
    pkg.cpp
    self_decrypt.cpp
    trax.cpp
    third_party/miniz/miniz.c
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp
)
target_include_directories(NHLLauncher PRIVATE ${IMGUI} third_party/miniz)
target_compile_definitions(NHLLauncher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(NHLLauncher PRIVATE d3d11 dxgi d3dcompiler bcrypt ole32 oleaut32 shell32 dwmapi windowscodecs)

# Dev tool: list / dump an ISO with the installer's reader.
add_executable(test_iso test_iso.cpp iso9660.cpp)
target_compile_definitions(test_iso PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(test_pkg test_pkg.cpp pkg.cpp)
target_compile_definitions(test_pkg PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(test_pkg PRIVATE bcrypt)
