cmake_minimum_required(VERSION 3.25)

project(UltraLightBrowser
    VERSION 1.1.2
    DESCRIPTION "Minimal, Ultra-Fast Windows 11 Native Browser Shell"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Ensure Unicode and Win32 definitions
add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00 # Target Windows 10/11
    WINVER=0x0A00
)

# Compiler & Linker optimizations for MSVC
if(MSVC)
    add_compile_options(
        /O2                 # Maximum optimization (favor speed)
        /Oi                 # Enable intrinsic functions
        /Ot                 # Favor fast code
        /GL                 # Whole program optimization
        /Gy                 # Enable function-level linking
        /utf-8              # Set source and execution character sets to UTF-8
        /permissive-        # Strict standard conformance
        /EHsc               # Standard C++ stack unwinding
        /W4                 # Warning level 4
        /Zc:inline          # Remove unreferenced functions/data
        /Zc:preprocessor    # Conforming preprocessor
    )
    add_link_options(
        /LTCG               # Link-time code generation
        /OPT:REF            # Eliminate unreferenced functions/data
        /OPT:ICF            # Identical COMDAT folding (reduce binary size)
        /SUBSYSTEM:WINDOWS  # Pure Win32 GUI application
        /MANIFEST:NO        # Custom manifest embedded via resource.rc
    )
endif()

# Locate Dependencies (vcpkg or NuGet)
find_package(wil CONFIG QUIET)
find_package(nlohmann_json CONFIG QUIET)

# Source files
set(SOURCE_FILES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/WebViewManager.cpp
    src/WebViewManager.hpp
    src/ExtensionManager.cpp
    src/ExtensionManager.hpp
    src/ElementBlocker.cpp
    src/ElementBlocker.hpp
    src/PowerManager.cpp
    src/PowerManager.hpp
    src/Config.cpp
    src/Config.hpp
    src/StringUtils.hpp
    resources/resource.rc
)

add_executable(UltraLightBrowser WIN32 ${SOURCE_FILES})

# NuGet Package directory fallback for Microsoft.Web.WebView2 & WIL
set(NUGET_PACKAGES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packages" CACHE PATH "Path to restored NuGet packages")
file(GLOB WEBVIEW2_DIRS "${NUGET_PACKAGES_DIR}/Microsoft.Web.WebView2.*")
file(GLOB WIL_DIRS "${NUGET_PACKAGES_DIR}/Microsoft.Windows.ImplementationLibrary.*")

if(WEBVIEW2_DIRS)
    list(GET WEBVIEW2_DIRS -1 LATEST_WEBVIEW2_DIR)
    target_include_directories(UltraLightBrowser PRIVATE "${LATEST_WEBVIEW2_DIR}/build/native/include")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_link_directories(UltraLightBrowser PRIVATE "${LATEST_WEBVIEW2_DIR}/build/native/x64")
        if(EXISTS "${LATEST_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")
            target_link_libraries(UltraLightBrowser PRIVATE "${LATEST_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")
        else()
            target_link_libraries(UltraLightBrowser PRIVATE WebView2Loader.dll.lib)
        endif()
    else()
        target_link_directories(UltraLightBrowser PRIVATE "${LATEST_WEBVIEW2_DIR}/build/native/x86")
        target_link_libraries(UltraLightBrowser PRIVATE WebView2Loader.dll.lib)
    endif()
    
    # Auto-copy WebView2Loader.dll to output directory if present
    if(EXISTS "${LATEST_WEBVIEW2_DIR}/build/native/x64/WebView2Loader.dll")
        add_custom_command(TARGET UltraLightBrowser POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LATEST_WEBVIEW2_DIR}/build/native/x64/WebView2Loader.dll"
            $<TARGET_FILE_DIR:UltraLightBrowser>
            COMMENT "Copying WebView2Loader.dll to target directory"
        )
    endif()
endif()

if(WIL_DIRS)
    list(GET WIL_DIRS -1 LATEST_WIL_DIR)
    target_include_directories(UltraLightBrowser PRIVATE "${LATEST_WIL_DIR}/include")
endif()

file(GLOB NLOHMANN_DIRS "${NUGET_PACKAGES_DIR}/nlohmann.json.*")
if(NLOHMANN_DIRS)
    list(GET NLOHMANN_DIRS -1 LATEST_NLOHMANN_DIR)
    target_include_directories(UltraLightBrowser PRIVATE "${LATEST_NLOHMANN_DIR}/build/native/include")
endif()

# Include directories
target_include_directories(UltraLightBrowser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link Libraries
if(TARGET WIL::WIL)
    target_link_libraries(UltraLightBrowser PRIVATE WIL::WIL)
endif()

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(UltraLightBrowser PRIVATE nlohmann_json::nlohmann_json)
endif()

target_link_libraries(UltraLightBrowser PRIVATE
    dwmapi.lib
    shlwapi.lib
    comctl32.lib
    version.lib
    user32.lib
    gdi32.lib
    ole32.lib
    oleaut32.lib
    shell32.lib
    advapi32.lib
    uxtheme.lib
    wininet.lib
)

# Embed Manifest
set_target_properties(UltraLightBrowser PROPERTIES
    VS_DPI_AWARE "PerMonitor"
)
