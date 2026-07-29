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

target_include_directories(WEW_scorpion PRIVATE "${CMAKE_SOURCE_DIR}")

target_link_options(WEW_scorpion PRIVATE
    -T ${LINKER_SCRIPT}
    -nostartfiles
    -nostdlib
    -Wl,-Map,scorpion.map
)

add_custom_command(TARGET WEW_scorpion POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:WEW_scorpion> $<TARGET_FILE_DIR:WEW_scorpion>/WEW_scorpion.bin
    COMMENT "Generated WEW_scorpion.bin and section sizes"
)
