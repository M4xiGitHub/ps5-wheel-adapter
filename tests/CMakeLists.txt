cmake_minimum_required(VERSION 3.13)
project(ps5_adapter_host_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()

set(FW ${CMAKE_CURRENT_LIST_DIR}/../firmware/src)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)
include_directories(${FW} ${CMAKE_CURRENT_LIST_DIR})

add_library(fwcore STATIC
    ${FW}/auth.c ${FW}/crc32.c ${FW}/devices.c ${FW}/diag.c
    ${FW}/g29.c ${FW}/hid_desc.c ${FW}/input_map.c ${FW}/ffb.c)

foreach(t test_hid_g29 test_input_routing test_auth test_diag test_ffb)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} fwcore)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME test_tools
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/test_tools.py)
endif()
