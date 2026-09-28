cmake_minimum_required(VERSION 3.20)

# megaprovider-host: the resident process behind the PowerShell provider. It owns one
# logged-in MegaApi and answers JSON requests over a named pipe (docs/HOST.md).
project(MegaProviderHost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MEGA C++ SDK, pinned as a submodule at the same commit MegaExplorer uses.
# FFmpeg is off: it only feeds video thumbnails on upload, and it is the one
# dependency the overlay triplet builds as a DLL that would have to ship next to the exe.
set(USE_FFMPEG OFF CACHE BOOL "" FORCE)
set(ENABLE_SDKLIB_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SDKLIB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_JAVA_BINDINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/sdk)

add_executable(megaprovider-host
    src/host/main.cpp
    src/host/Platform.h
    src/host/Service.h
    src/host/Service.cpp
    src/mega/Handles.h
    src/mega/Handles.cpp
    src/app/Logging.h
    src/app/Logging.cpp
    src/mega/MegaSdkClient.h
    src/mega/MegaSdkClient.cpp
    src/mega/MegaSdkListeners.h
    src/mega/MegaSdkLogger.h
    src/mega/MegaSdkLogger.cpp
    src/mega/SerialExecutor.h
)
target_include_directories(megaprovider-host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${CMAKE_CURRENT_LIST_DIR}/third_party/json)
target_link_libraries(megaprovider-host PRIVATE MEGA::SDKlib)
if(WIN32)
    target_sources(megaprovider-host PRIVATE src/host/Platform_win.cpp)
    target_compile_definitions(megaprovider-host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(megaprovider-host PRIVATE /W4 /external:W0 /utf-8)
    target_link_libraries(megaprovider-host PRIVATE advapi32)
else()
    target_sources(megaprovider-host PRIVATE src/host/Platform_unix.cpp)
    # The SDK's own choice for Unix (sdklib_target.cmake), minus -Wconversion.
    target_compile_options(megaprovider-host PRIVATE -Wall -Wextra)
endif()
