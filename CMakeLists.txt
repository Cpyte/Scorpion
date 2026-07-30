cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk-2.3.0")
set(PICO_PLATFORM rp2350-riscv)
set(PICO_BOARD pico2)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(Scorpion C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -ffreestanding
    -Wall -Wextra -Wpedantic
)

if(NOT ARCH)
    set(ARCH riscv32)
endif()

set(ARCH_DIR "${CMAKE_SOURCE_DIR}/arch/${ARCH}")
set(LINKER_SCRIPT "${ARCH_DIR}/kernel.ld")

find_program(RISCV_LD riscv64-elf-ld REQUIRED)

# --- User test SEF binary ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_user.o
    COMMAND ${CMAKE_C_COMPILER} -march=rv32imac -mabi=ilp32
        -nostdlib -ffreestanding -c
        ${CMAKE_SOURCE_DIR}/user/test_user.S
        -o ${CMAKE_CURRENT_BINARY_DIR}/test_user.o
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test_user.S
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_user.elf
    COMMAND ${RISCV_LD} -m elf32lriscv -e _start --no-relax
        -Ttext=0 -o ${CMAKE_CURRENT_BINARY_DIR}/test_user.elf
        ${CMAKE_CURRENT_BINARY_DIR}/test_user.o
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test_user.o
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_user.sef
    COMMAND python3 ${CMAKE_SOURCE_DIR}/user/mksef.py
        ${CMAKE_CURRENT_BINARY_DIR}/test_user.elf
        ${CMAKE_CURRENT_BINARY_DIR}/test_user.sef
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test_user.elf
)

# --- Controller SEF binary (depends on test_user.sef) ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/controller.o
    COMMAND ${CMAKE_C_COMPILER} -march=rv32imac -mabi=ilp32
        -nostdlib -ffreestanding -c
        -I${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/user/controller.S
        -o ${CMAKE_CURRENT_BINARY_DIR}/controller.o
    DEPENDS ${CMAKE_SOURCE_DIR}/user/controller.S
            ${CMAKE_CURRENT_BINARY_DIR}/test_user.sef
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/controller.elf
    COMMAND ${RISCV_LD} -m elf32lriscv -e _start --no-relax
        -Ttext=0 -o ${CMAKE_CURRENT_BINARY_DIR}/controller.elf
        ${CMAKE_CURRENT_BINARY_DIR}/controller.o
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/controller.o
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/controller.sef
    COMMAND python3 ${CMAKE_SOURCE_DIR}/user/mksef.py --flags 1
        ${CMAKE_CURRENT_BINARY_DIR}/controller.elf
        ${CMAKE_CURRENT_BINARY_DIR}/controller.sef
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/controller.elf
)

# Ensure controller.sef is built before the kernel links
add_custom_target(controller_sef_target DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/controller.sef)

add_executable(WEW_scorpion
    main.c
    alloc.c
    palloc.c
    lifecycle.c
    loader.c
    context.c
    trap.c
    console.c
    driver.c
    flash.c
    fuse.c
    "${ARCH_DIR}/context.S"
    "${ARCH_DIR}/trap.S"
    "${ARCH_DIR}/crt0.S"
    abi/scorpion.h
)
add_dependencies(WEW_scorpion controller_sef_target)

target_include_directories(WEW_scorpion PRIVATE "${CMAKE_SOURCE_DIR}")

target_link_options(WEW_scorpion PRIVATE
    -T ${LINKER_SCRIPT}
    -nostartfiles
    -nostdlib
    -Wl,-Map,scorpion.map
)

add_custom_command(TARGET WEW_scorpion POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:WEW_scorpion> $<TARGET_FILE_DIR:WEW_scorpion>/WEW_scorpion.bin
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/packrom.py
        $<TARGET_FILE_DIR:WEW_scorpion>/WEW_scorpion.bin
        ${CMAKE_CURRENT_BINARY_DIR}/controller.sef
        $<TARGET_FILE_DIR:WEW_scorpion>/Scorpion.uf2
    COMMENT "Packaged Scorpion.uf2 (boot2+kernel+controller)"
)
