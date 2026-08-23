add_executable(tcl_demo tcl_demo.cpp)
target_link_libraries(tcl_demo PRIVATE procinvoker Qt5::Core)
target_compile_definitions(tcl_demo PRIVATE
    CALC_TCL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/calc.tcl"
    CALC_PROCS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/calc_procs.tcl"
)

# 嵌入式 Tcl 宿主示例（需要 tcl 开发包，如 apt install tcl-dev）
find_package(TCL)
if(TCL_FOUND)
    add_executable(embedded_host embedded_host.c)
    target_include_directories(embedded_host PRIVATE ${TCL_INCLUDE_PATH})
    target_link_libraries(embedded_host PRIVATE ${TCL_LIBRARY})
    target_compile_definitions(tcl_demo PRIVATE
        EMBEDDED_HOST_PATH="$<TARGET_FILE:embedded_host>")
else()
    message(STATUS "Tcl 开发包未找到，跳过 embedded_host 示例（tcl_demo 的 --embedded 模式不可用）")
endif()
