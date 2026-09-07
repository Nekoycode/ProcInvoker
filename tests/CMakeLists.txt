# Test 组件仅测试需要，下沉到此处（库消费者不应被迫安装 Qt5Test）
find_package(Qt5 5.15 REQUIRED COMPONENTS Test)

add_executable(repl_fixture fixture/repl_fixture.cpp)

add_executable(tst_framer tst_framer.cpp)
target_include_directories(tst_framer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_framer PRIVATE procinvoker Qt5::Test)
add_test(NAME framer COMMAND tst_framer)

add_executable(tst_promptframer tst_promptframer.cpp)
target_include_directories(tst_promptframer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_promptframer PRIVATE procinvoker Qt5::Test)
add_test(NAME promptframer COMMAND tst_promptframer)

# fixture 绝对路径经 file(GENERATE) 生成头文件注入（对空格路径/Windows 安全）
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fixture_path.h"
    CONTENT "#pragma once\n#define FIXTURE_PATH \"$<TARGET_FILE:repl_fixture>\"\n"
)

add_executable(tst_invoker tst_invoker.cpp)
add_dependencies(tst_invoker repl_fixture)
target_include_directories(tst_invoker PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(tst_invoker PRIVATE procinvoker Qt5::Test)
add_test(NAME invoker COMMAND tst_invoker)
