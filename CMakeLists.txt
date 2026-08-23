cmake_minimum_required(VERSION 3.16)
project(ProcInvoker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Test)

add_library(procinvoker
    include/procinvoker/ProcInvoker.h
    src/ProcInvoker.cpp
    src/ProcInvokerCore.cpp
    src/ProcInvokerCore.h
    src/MarkerFramer.cpp
    src/MarkerFramer.h
    src/PromptFramer.cpp
    src/PromptFramer.h
)
target_include_directories(procinvoker
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(procinvoker PUBLIC Qt5::Core)

enable_testing()
add_subdirectory(tests)

option(PROCINVOKER_BUILD_EXAMPLES "Build examples" ON)
if(PROCINVOKER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
