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
        src/net/Acceptor.cpp
        src/net/Epoller.cpp
        src/net/EventLoop.cpp
        src/net/EventLoopThread.cpp
        src/net/EventLoopThreadPool.cpp
        src/net/TimerId.cpp
        src/net/TimerQueue.cpp
        src/net/TcpConnection.cpp
    )
else()
    message(STATUS "sys/epoll.h not found; Linux Reactor sources are excluded")
endif()

add_library(l4lb_net STATIC ${L4LB_NET_SOURCES})
target_include_directories(l4lb_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(l4lb_net PUBLIC l4lb_base Threads::Threads)
l4lb_set_warnings(l4lb_net)
l4lb_enable_sanitizers(l4lb_net)

add_library(l4lb_lb STATIC
    src/base/AsyncLogger.cpp
    src/lb/Backend.cpp
    src/lb/BackendPool.cpp
    src/lb/Connector.cpp
    src/lb/HealthChecker.cpp
    src/lb/LoadBalancer.cpp
    src/lb/ProxySession.cpp
    src/lb/SessionMap.cpp
)
target_include_directories(l4lb_lb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(l4lb_lb PUBLIC l4lb_net)
l4lb_set_warnings(l4lb_lb)
l4lb_enable_sanitizers(l4lb_lb)

if(TARGET PkgConfig::JSONCPP)
    add_library(l4lb_config STATIC src/config/Config.cpp)
    target_include_directories(l4lb_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(l4lb_config PUBLIC PkgConfig::JSONCPP l4lb_lb)
    l4lb_set_warnings(l4lb_config)
    l4lb_enable_sanitizers(l4lb_config)
else()
    add_library(l4lb_config INTERFACE)
endif()

if(TARGET PkgConfig::JSONCPP)
    add_library(l4lb_app_core STATIC src/app/L4ProxyServer.cpp)
    target_include_directories(l4lb_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(l4lb_app_core PUBLIC l4lb_config l4lb_lb)
    l4lb_set_warnings(l4lb_app_core)
    l4lb_enable_sanitizers(l4lb_app_core)
endif()

add_executable(l4lb_server src/app/main.cpp)
if(TARGET PkgConfig::JSONCPP)
    target_compile_definitions(l4lb_server PRIVATE L4LB_HAS_CONFIG=1)
    target_link_libraries(l4lb_server PRIVATE l4lb_app_core)
else()
    target_link_libraries(l4lb_server PRIVATE l4lb_lb l4lb_config)
endif()
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
        list(APPEND L4LB_UNIT_TEST_SOURCES
            tests/unit/EventLoopTest.cpp
            tests/unit/AsyncLoggerTest.cpp
            tests/unit/EventLoopThreadTest.cpp
            tests/unit/AcceptorTest.cpp
            tests/unit/TimerQueueTest.cpp
            tests/unit/TcpConnectionTest.cpp
            tests/unit/ConnectorTest.cpp
            tests/unit/LoadBalancerTest.cpp
            tests/unit/HealthCheckerTest.cpp
            tests/unit/ProxySessionTest.cpp
        )
        if(TARGET PkgConfig::JSONCPP)
            list(APPEND L4LB_UNIT_TEST_SOURCES
                tests/unit/ConfigTest.cpp
                tests/unit/L4ProxyServerTest.cpp
            )
        endif()
    endif()
    add_executable(l4lb_unit_tests ${L4LB_UNIT_TEST_SOURCES})
    target_link_libraries(l4lb_unit_tests PRIVATE l4lb_lb l4lb_config)
    if(TARGET PkgConfig::JSONCPP)
        target_link_libraries(l4lb_unit_tests PRIVATE l4lb_app_core)
    endif()
    l4lb_set_warnings(l4lb_unit_tests)
    l4lb_enable_sanitizers(l4lb_unit_tests)
    add_test(NAME l4lb_unit_tests COMMAND l4lb_unit_tests)
    if(TARGET PkgConfig::JSONCPP)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME l4lb_process_smoke
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/smoke.py
                    --server $<TARGET_FILE:l4lb_server>
                    --config ${CMAKE_CURRENT_SOURCE_DIR}/config/example.json
            )
            set_tests_properties(l4lb_process_smoke PROPERTIES RUN_SERIAL TRUE TIMEOUT 10)
        endif()
    endif()
endif()
