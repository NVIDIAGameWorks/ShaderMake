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

project (ShaderMake LANGUAGES C CXX)

# Globals?
set_property (GLOBAL PROPERTY USE_FOLDERS ON)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_C_STANDARD 99)

if (MSVC)
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

# Compile options
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set (COMPILE_OPTIONS -msse4.1 -Wextra)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set (COMPILE_OPTIONS -msse4.1 -Wextra)
elseif (MSVC)
    set (COMPILE_OPTIONS /W4 /WX /wd4324)
else ()
    message (WARNING "Unknown compiler!")
endif ()

# Library with blob read and write functions for use by client projects
add_library (ShaderMakeBlob STATIC "src/ShaderBlob.cpp" "include/ShaderMake/ShaderBlob.h")
target_include_directories (ShaderMakeBlob PUBLIC "include")
set_property (TARGET ShaderMakeBlob PROPERTY FOLDER ShaderMake)

# ShaderMake executable
file (GLOB GLOB_SOURCE "src/*.cpp" "src/*.c" "src/*.h" "include/ShaderMake/*.h")
source_group ("Source" FILES ${GLOB_SOURCE})

add_executable (ShaderMake ${GLOB_SOURCE})
target_compile_options (ShaderMake PRIVATE ${COMPILE_OPTIONS})
set_target_properties (ShaderMake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SHADERMAKE_BIN_OUTPUT_PATH}/$<CONFIG>")
set_property (DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ShaderMake)
set_property (TARGET ShaderMake PROPERTY FOLDER ShaderMake)
target_link_libraries (ShaderMake ShaderMakeBlob)

if (MSVC)
    target_compile_definitions (ShaderMake PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_options (ShaderMake PRIVATE "/DELAYLOAD:dxcompiler.dll")
    target_link_libraries (ShaderMake d3dcompiler dxcompiler delayimp)
else ()
    target_link_libraries (ShaderMake stdc++fs pthread)
endif ()

if (SHADERMAKE_SEARCH_FOR_COMPILERS)
    # Finding FXC/DXC
    if (WIN32)
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

        find_program (FXC_PATH "${WINDOWS_SDK_BIN}/fxc")
        find_program (DXC_PATH "${WINDOWS_SDK_BIN}/dxc")
        find_program (DXC_SPIRV_PATH "$ENV{VULKAN_SDK}/Bin/dxc")

        if (NOT FXC_PATH)
            message (FATAL_ERROR "Can't find FXC in WindowsSDK: ${WINDOWS_SDK_BIN}")
        endif ()

        if (NOT DXC_PATH)
            message (FATAL_ERROR "Can't find DXC in WindowsSDK: ${WINDOWS_SDK_BIN}")
        endif ()
    else ()
        # On Linux - VulkanSDK does not set VULKAN_SDK, but DXC can be called directly
        find_program (DXC_SPIRV_PATH "dxc")
    endif ()

    if (NOT DXC_SPIRV_PATH)
        find_program (DXC_SPIRV_PATH "dxc" "${DXC_CUSTOM_PATH}")
    endif ()

    if (NOT DXC_SPIRV_PATH)
        message (FATAL_ERROR "Can't find DXC: Specify custom path using 'DXC_CUSTOM_PATH' parameter or install VulkanSDK!")
    endif ()

    message (STATUS "Setting 'FXC_PATH' to '${FXC_PATH}'")
    message (STATUS "Setting 'DXC_PATH' to '${DXC_PATH}'")
    message (STATUS "Setting 'DXC_SPIRV_PATH' to '${DXC_SPIRV_PATH}'")
endif()
