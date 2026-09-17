cmake_minimum_required(VERSION 3.16)

project(l4lb VERSION 0.1.0 LANGUAGES CXX)

option(L4LB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(L4LB_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(L4LB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(L4LB_REQUIRE_JSONCPP "Fail configuration when jsoncpp is unavailable" OFF)

if(L4LB_ENABLE_TSAN AND (L4LB_ENABLE_ASAN OR L4LB_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with ASan/UBSan")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
include(CheckIncludeFileCXX)
check_include_file_cxx("sys/epoll.h" L4LB_HAVE_EPOLL)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(JSONCPP QUIET IMPORTED_TARGET jsoncpp)
endif()

if(L4LB_REQUIRE_JSONCPP AND NOT TARGET PkgConfig::JSONCPP)
    message(FATAL_ERROR "jsoncpp is required; install its development package")
elseif(NOT TARGET PkgConfig::JSONCPP)
    message(STATUS "jsoncpp not found; config implementation is not built yet")
endif()

function(l4lb_set_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

function(l4lb_enable_sanitizers target)
    if(L4LB_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    if(L4LB_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
    if(L4LB_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

add_library(l4lb_base INTERFACE)
target_include_directories(l4lb_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(L4LB_NET_SOURCES
    src/net/Buffer.cpp
    src/net/InetAddress.cpp
    src/net/Socket.cpp
    src/net/UniqueFd.cpp
)

if(L4LB_HAVE_EPOLL)
    list(APPEND L4LB_NET_SOURCES
        src/net/Channel.cpp
        src/net/Epoller.cpp
        src/net/EventLoop.cpp
    )
else()
    message(STATUS "sys/epoll.h not found; Linux Reactor sources are excluded")
endif()

add_library(l4lb_net STATIC ${L4LB_NET_SOURCES})
target_include_directories(l4lb_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(l4lb_net PUBLIC l4lb_base Threads::Threads)
l4lb_set_warnings(l4lb_net)
l4lb_enable_sanitizers(l4lb_net)

add_library(l4lb_lb INTERFACE)
target_link_libraries(l4lb_lb INTERFACE l4lb_net)

add_library(l4lb_config INTERFACE)
if(TARGET PkgConfig::JSONCPP)
    target_link_libraries(l4lb_config INTERFACE PkgConfig::JSONCPP)
endif()

add_executable(l4lb_server src/app/main.cpp)
target_link_libraries(l4lb_server PRIVATE l4lb_lb l4lb_config)
l4lb_set_warnings(l4lb_server)
l4lb_enable_sanitizers(l4lb_server)

include(CTest)
if(BUILD_TESTING)
    set(L4LB_UNIT_TEST_SOURCES
        tests/unit/BufferTest.cpp
        tests/unit/SocketTest.cpp
        tests/unit/TestMain.cpp
        tests/unit/UniqueFdTest.cpp
    )
    if(L4LB_HAVE_EPOLL)
        list(APPEND L4LB_UNIT_TEST_SOURCES tests/unit/EventLoopTest.cpp)
    endif()
    add_executable(l4lb_unit_tests ${L4LB_UNIT_TEST_SOURCES})
    target_link_libraries(l4lb_unit_tests PRIVATE l4lb_net)
    l4lb_set_warnings(l4lb_unit_tests)
    l4lb_enable_sanitizers(l4lb_unit_tests)
    add_test(NAME l4lb_unit_tests COMMAND l4lb_unit_tests)
endif()
