cmake_minimum_required(VERSION 3.21)
project(mc_legacy_render_backport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RENDER_BRIDGE_BUILD_RUNTIME "Build the runtime library linked against the Vulkan loader" ON)
option(RENDER_BRIDGE_BUILD_TESTS   "Build the mock-backed JNI test library and test suite"      ON)

# Minecraft 1.17.1 runs on Java 16, so the Java side targets release 16.
set(RENDER_BRIDGE_JAVA_RELEASE 16)

find_package(Java 16 REQUIRED COMPONENTS Development Runtime)
include(UseJava)
find_package(JNI REQUIRED)

# ---------------------------------------------------------------------------
# Vulkan: the runtime library needs the loader; the test library only needs
# headers, which are fetched (pinned) when no system SDK is installed.
# ---------------------------------------------------------------------------
if(RENDER_BRIDGE_BUILD_RUNTIME)
    find_package(Vulkan REQUIRED)
else()
    find_package(Vulkan QUIET)
endif()
if(NOT TARGET Vulkan::Headers)
    include(FetchContent)
    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG        v1.4.366
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(VulkanHeaders)
endif()

if(MSVC)
    set(RENDER_BRIDGE_WARNINGS /W4 /WX)
else()
    set(RENDER_BRIDGE_WARNINGS
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        -Wold-style-cast -Wnon-virtual-dtor -Wcast-qual -Werror)
endif()

function(render_bridge_configure target output_dir)
    target_include_directories(${target} PRIVATE native/include ${JNI_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE Vulkan::Headers)
    target_compile_options(${target} PRIVATE ${RENDER_BRIDGE_WARNINGS})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME              vulkan_render_bridge   # System.loadLibrary("vulkan_render_bridge")
        CXX_VISIBILITY_PRESET    hidden                 # only JNIEXPORT symbols are exported
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY ${output_dir}
        RUNTIME_OUTPUT_DIRECTORY ${output_dir})
endfunction()

# ---------------------------------------------------------------------------
# Java API
# ---------------------------------------------------------------------------
set(CMAKE_JAVA_COMPILE_FLAGS -Xlint:all -Werror --release ${RENDER_BRIDGE_JAVA_RELEASE})
add_jar(render_bridge_java
    SOURCES     java/src/main/java/dev/renderbackport/vulkan/RenderGraphPass.java
    OUTPUT_NAME render-bridge
    OUTPUT_DIR  ${CMAKE_BINARY_DIR}/jars)

# ---------------------------------------------------------------------------
# Runtime native library (links the real Vulkan loader)
# ---------------------------------------------------------------------------
if(RENDER_BRIDGE_BUILD_RUNTIME)
    add_library(vulkan_render_bridge SHARED native/src/RenderGraphPassJni.cpp)
    render_bridge_configure(vulkan_render_bridge ${CMAKE_BINARY_DIR}/lib)
    target_link_libraries(vulkan_render_bridge PRIVATE Vulkan::Vulkan)
endif()

# ---------------------------------------------------------------------------
# Tests: the same bridge source, linked with a mock RenderGraph and stub
# vkCmd* entry points instead of the loader, driven from Java.
# ---------------------------------------------------------------------------
if(RENDER_BRIDGE_BUILD_TESTS)
    enable_testing()

    add_library(vulkan_render_bridge_test SHARED
        native/src/RenderGraphPassJni.cpp
        native/test/MockGraph.cpp)
    render_bridge_configure(vulkan_render_bridge_test ${CMAKE_BINARY_DIR}/test-lib)

    add_jar(render_bridge_tests
        SOURCES      java/src/test/java/dev/renderbackport/vulkan/BridgeTest.java
        INCLUDE_JARS render_bridge_java
        OUTPUT_NAME  render-bridge-tests
        OUTPUT_DIR   ${CMAKE_BINARY_DIR}/jars)

    get_target_property(_main_jar render_bridge_java  JAR_FILE)
    get_target_property(_test_jar render_bridge_tests JAR_FILE)
    if(WIN32)
        set(_cp "${_main_jar}\;${_test_jar}")
    else()
        set(_cp "${_main_jar}:${_test_jar}")
    endif()

    add_test(NAME jni_bridge
        COMMAND ${Java_JAVA_EXECUTABLE}
                -Xcheck:jni
                -Djava.library.path=${CMAKE_BINARY_DIR}/test-lib
                -cp ${_cp}
                dev.renderbackport.vulkan.BridgeTest)
    set_tests_properties(jni_bridge PROPERTIES
        PASS_REGULAR_EXPRESSION "ALL [0-9]+ CHECKS PASSED"
        FAIL_REGULAR_EXPRESSION "WARNING in native method|FATAL ERROR|FAILED:"
        TIMEOUT 120)
endif()
