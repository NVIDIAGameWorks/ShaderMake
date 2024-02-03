cmake_minimum_required (VERSION 3.15)

# Is submodule?
if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
    set (SHADERMAKE_IS_SUBMODULE OFF)
else ()
    set (SHADERMAKE_IS_SUBMODULE ON)
endif ()

# Cached
if (NOT SHADERMAKE_IS_SUBMODULE)
    set (CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "")
endif()

set (SHADERMAKE_BIN_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/bin" CACHE STRING "")
set (SHADERMAKE_SEARCH_FOR_COMPILERS ${SHADERMAKE_IS_SUBMODULE} CACHE BOOL "Toggles whether to search for dxc.exe and fxc.exe")
option (SHADERMAKE_FIND_FXC "Toggles whether to search for FXC" ON)
option (SHADERMAKE_FIND_DXC "Toggles whether to search for DXC for DXIL" ON)
option (SHADERMAKE_FIND_DXC_SPIRV "Toggles whether to search for DXC for SPIR-V" ON)

project (ShaderMake LANGUAGES C CXX)

# Globals?
set_property (GLOBAL PROPERTY USE_FOLDERS ON)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_C_STANDARD 99)

if (MSVC)
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # Test the Windows SDK version to make sure that we can compile ShaderMake.
    # ShaderMake uses some relatively new APIs for DXC, and there's no straightforward way to test
    # if they're declared in the SDK headers. So, test it here to prevent obscure errors.
    set(_MIN_WINDOWS_SDK_VERSION_BUILD 20348)
    string(REGEX MATCH "^10\\.0\\.([0-9]+)\\.[0-9]+$" _WINDOWS_SDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    if (_WINDOWS_SDK_VERSION)
        set(_WINDOWS_SDK_VERSION_BUILD "${CMAKE_MATCH_1}")
        if (_WINDOWS_SDK_VERSION_BUILD LESS _MIN_WINDOWS_SDK_VERSION_BUILD)
            message(SEND_ERROR "ShaderMake requires Windows SDK version at least 10.0.${_MIN_WINDOWS_SDK_VERSION_BUILD}.0.")
        endif()
    else()
        message(WARNING "ShaderMake: Unknown Windows SDK version '${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}', errors may occur during build.")
    endif()
endif()

# Compile options
if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
  set (SIMD -msse4.1)
elseif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
  set (SIMD -fPIC)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set (COMPILE_OPTIONS ${SIMD} -Wextra)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set (COMPILE_OPTIONS ${SIMD} -Wextra)
elseif (MSVC)
    set (COMPILE_OPTIONS /W4 /WX /wd4324)
else ()
    message (WARNING "Unknown compiler!")
endif ()

# Library with blob read and write functions for use by client projects
add_library (ShaderMakeBlob STATIC
    include/ShaderMake/ShaderBlob.h
    src/ShaderBlob.cpp
)
target_include_directories (ShaderMakeBlob PUBLIC "include")
target_compile_options (ShaderMakeBlob PRIVATE ${COMPILE_OPTIONS})
set_property (TARGET ShaderMakeBlob PROPERTY FOLDER ShaderMake)

# ShaderMake executable
add_executable (ShaderMake
    src/argparse.c
    src/argparse.h
    src/ShaderMake.cpp 
)
target_compile_options (ShaderMake PRIVATE ${COMPILE_OPTIONS})
set_target_properties (ShaderMake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SHADERMAKE_BIN_OUTPUT_PATH}/$<CONFIG>")
set_property (DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ShaderMake)
set_property (TARGET ShaderMake PROPERTY FOLDER ShaderMake)
target_link_libraries (ShaderMake ShaderMakeBlob)

if (MSVC)
    target_compile_definitions (ShaderMake PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_options (ShaderMake PRIVATE "/DELAYLOAD:dxcompiler.dll")
    target_link_libraries (ShaderMake d3dcompiler dxcompiler delayimp)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_link_libraries (ShaderMake pthread)
else ()
    target_link_libraries (ShaderMake stdc++fs pthread)
endif ()

if (SHADERMAKE_SEARCH_FOR_COMPILERS)
    # Finding FXC/DXC
    if (WIN32)
        if (SHADERMAKE_FIND_FXC OR SHADERMAKE_FIND_DXC)
            # On Windows - FXC and DXC are part of WindowsSDK and there's also DXC in VulkanSDK which supports SPIR-V
            if (DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
                set (WINDOWS_SDK_VERSION ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})
            elseif (DEFINED ENV{WindowsSDKLibVersion})
                string (REGEX REPLACE "\\\\$" "" WINDOWS_SDK_VERSION "$ENV{WindowsSDKLibVersion}")
            else ()
                message (FATAL_ERROR "WindowsSDK is not installed (CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION is not defined; WindowsSDKLibVersion is '$ENV{WindowsSDKLibVersion}')!")
            endif ()

            get_filename_component (WINDOWS_SDK_ROOT "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
            set (WINDOWS_SDK_BIN "${WINDOWS_SDK_ROOT}/bin/${WINDOWS_SDK_VERSION}/x64")

            if (SHADERMAKE_FIND_FXC)
                find_program (FXC_PATH "${WINDOWS_SDK_BIN}/fxc")
                if (NOT FXC_PATH)
                    message (FATAL_ERROR "Can't find FXC in WindowsSDK: ${WINDOWS_SDK_BIN}")
                endif ()
            endif()

            if (SHADERMAKE_FIND_DXC)
                find_program (DXC_PATH "${WINDOWS_SDK_BIN}/dxc")
                if (NOT DXC_PATH)
                    message (FATAL_ERROR "Can't find DXC in WindowsSDK: ${WINDOWS_SDK_BIN}")
                endif ()
            endif()
        endif()

        if (SHADERMAKE_FIND_DXC_SPIRV)
            find_program (DXC_SPIRV_PATH "$ENV{VULKAN_SDK}/Bin/dxc")
        endif()
    else ()
        if (SHADERMAKE_FIND_DXC_SPIRV)
            # On Linux - VulkanSDK does not set VULKAN_SDK, but DXC can be called directly
            find_program (DXC_SPIRV_PATH "dxc")
        endif()
    endif ()

    if (SHADERMAKE_FIND_DXC_SPIRV AND NOT DXC_SPIRV_PATH)
        find_program (DXC_SPIRV_PATH "dxc" "${DXC_CUSTOM_PATH}")
    endif ()

    if (SHADERMAKE_FIND_DXC_SPIRV AND NOT DXC_SPIRV_PATH)
        message (FATAL_ERROR "Can't find DXC: Specify custom path using 'DXC_CUSTOM_PATH' parameter or install VulkanSDK!")
    endif ()

    if (SHADERMAKE_FIND_FXC)
        message (STATUS "Setting 'FXC_PATH' to '${FXC_PATH}'")
    endif()
    if (SHADERMAKE_FIND_DXC)
        message (STATUS "Setting 'DXC_PATH' to '${DXC_PATH}'")
    endif()
    if (SHADERMAKE_FIND_DXC_SPIRV)
        message (STATUS "Setting 'DXC_SPIRV_PATH' to '${DXC_SPIRV_PATH}'")
    endif()
endif()
