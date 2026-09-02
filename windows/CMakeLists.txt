cmake_minimum_required(VERSION 3.20)
project(DKChatebras LANGUAGES CXX)

add_executable(DKChatebras WIN32
    src/main.cpp
    src/storage.cpp
    src/network.cpp
    src/markdown.cpp
    src/app.manifest
)

target_compile_features(DKChatebras PRIVATE cxx_std_17)
target_compile_definitions(DKChatebras PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0601 WINVER=0x0601
)
if(MSVC)
    set_property(TARGET DKChatebras PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(DKChatebras PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(DKChatebras PRIVATE /SUBSYSTEM:WINDOWS,6.01)
endif()

target_link_libraries(DKChatebras PRIVATE
    comctl32 crypt32 dwmapi gdi32 ole32 shell32 user32 winhttp
)
set_target_properties(DKChatebras PROPERTIES
    OUTPUT_NAME "DKChatebras"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
