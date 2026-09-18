cmake_minimum_required(VERSION 3.16)
project(l4lb LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "l4lb uses Linux epoll; please build inside Linux/WSL2")
endif()
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

option(L4LB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(L4LB_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
if(L4LB_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(L4LB_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

find_package(Threads REQUIRED)
add_library(l4lb_core
    src/net/UniqueFd.cpp
    src/net/Socket.cpp
    src/net/InetAddress.cpp
    src/net/Buffer.cpp
    src/net/Channel.cpp
    src/net/Epoller.cpp
    src/net/EventLoop.cpp
    src/net/Acceptor.cpp
    src/net/TimerId.cpp
    src/net/TimerQueue.cpp
    src/net/TcpConnection.cpp
    src/lb/Connector.cpp
    src/lb/ProxySession.cpp
    src/lb/LoadBalancer.cpp
)
target_include_directories(l4lb_core PUBLIC src)
# EventLoop 保留线程归属断言/唤醒基础能力；默认服务不创建 worker。
target_link_libraries(l4lb_core PUBLIC Threads::Threads)

add_executable(l4lb_server src/app/main.cpp)
target_link_libraries(l4lb_server PRIVATE l4lb_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(l4lb_unit_tests
        tests/unit/TestMain.cpp
        tests/unit/BufferTest.cpp
        tests/unit/UniqueFdTest.cpp
        tests/unit/SocketTest.cpp
        tests/unit/EventLoopTest.cpp
        tests/unit/AcceptorTest.cpp
        tests/unit/TimerQueueTest.cpp
        tests/unit/TcpConnectionTest.cpp
        tests/unit/ConnectorTest.cpp
        tests/unit/ProxySessionTest.cpp
        tests/unit/LoadBalancerTest.cpp
    )
    target_link_libraries(l4lb_unit_tests PRIVATE l4lb_core)
    add_test(NAME l4lb_unit_tests COMMAND l4lb_unit_tests)
    set_tests_properties(l4lb_unit_tests PROPERTIES TIMEOUT 30)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME l4lb_process_smoke COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/smoke.py
        --server $<TARGET_FILE:l4lb_server>)
    set_tests_properties(l4lb_process_smoke PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
endif()
