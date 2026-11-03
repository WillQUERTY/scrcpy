cmake_minimum_required(VERSION 3.20)
project(scrcpy-vcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000B
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(scrcpy-mfvcam SHARED
    src/mfvcam_source.cpp
    src/dllmain.cpp
    src/mfvcam.def
)

target_link_libraries(scrcpy-mfvcam PRIVATE
    mfplat
    mf
    mfuuid
    mfsensorgroup
    propsys
    shlwapi
    advapi32
    ole32
)

add_executable(scrcpy-vcam-ctl
    src/vcam_ctl.cpp
)

target_link_libraries(scrcpy-vcam-ctl PRIVATE
    mfplat
    mf
    mfreadwrite
    mfuuid
    mfsensorgroup
    advapi32
    ole32
    shell32
)
