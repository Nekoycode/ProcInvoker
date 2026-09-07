cmake_minimum_required(VERSION 3.16)
project(ProcInvoker VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core)

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
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(procinvoker PUBLIC Qt5::Core)

# 顶层项目才默认构建 tests/examples；作为子项目（add_subdirectory）引入时默认关闭。
# CMake 3.16 无 PROJECT_IS_TOP_LEVEL，用源码路径比较判断
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PROCINVOKER_TOP_LEVEL_DEFAULT ON)
else()
    set(PROCINVOKER_TOP_LEVEL_DEFAULT OFF)
endif()
option(PROCINVOKER_BUILD_TESTS "Build tests" ${PROCINVOKER_TOP_LEVEL_DEFAULT})
option(PROCINVOKER_BUILD_EXAMPLES "Build examples" ${PROCINVOKER_TOP_LEVEL_DEFAULT})

if(PROCINVOKER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(PROCINVOKER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 安装与导出：下游 find_package(ProcInvoker 1.0 REQUIRED) 后链接 ProcInvoker::procinvoker
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS procinvoker EXPORT ProcInvokerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ProcInvokerTargets
    FILE ProcInvokerTargets.cmake
    NAMESPACE ProcInvoker::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProcInvoker
)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProcInvokerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ProcInvokerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProcInvoker
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ProcInvokerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ProcInvokerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ProcInvokerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProcInvoker
)
