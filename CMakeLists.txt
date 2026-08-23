cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Platform selection
#
#   cmake -B build -DPLATFORM=rp2350    Raspberry Pi Pico 2 (Hazard3 RISC-V)
#   cmake -B build -DPLATFORM=esp32c3   ESP32-C3 (RV32IMC, experimental)
#   cmake -B build -DPLATFORM=esp32s3   ESP32-S3 (Xtensa LX7, call0 ABI)
#
set(PLATFORM "rp2350" CACHE STRING "Target platform: rp2350 | esp32c3 | esp32s3")

if(PLATFORM STREQUAL "rp2350")
    # Prefer the vendored Pico SDK's toolchain selection. If no 32-bit
    # RISC-V triple is installed (e.g. only homebrew's riscv64-elf),
    # fall back to a manual toolchain with the Hazard3 flags the SDK
    # would have chosen. No SDK libraries are linked either way.
    find_program(SCORPION_RV32_GCC
        NAMES riscv32-pico-elf-gcc riscv32-unknown-elf-gcc
              riscv32-corev-elf-gcc riscv-none-elf-gcc)
    if(SCORPION_RV32_GCC)
        set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk-2.3.0")
        set(PICO_PLATFORM rp2350-riscv)
        set(PICO_BOARD pico2)
        include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
        # Flags the SDK toolchain would have chosen; used for libgcc.
        set(ARCH_FLAGS
            "-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb" "-mabi=ilp32")
    else()
        # Cross-compile declaration keeps CMake from injecting host
        # flags (-arch, sysroot) into the bare-metal build.
        set(CMAKE_SYSTEM_NAME Generic)
        set(CMAKE_SYSTEM_PROCESSOR riscv)
        find_program(CMAKE_C_COMPILER riscv64-elf-gcc REQUIRED)
        find_program(CMAKE_ASM_COMPILER riscv64-elf-gcc REQUIRED)
        set(ARCH_FLAGS
            "-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb" "-mabi=ilp32")
        add_compile_options(${ARCH_FLAGS})
        message(STATUS "rp2350: using manual toolchain "
                       "${CMAKE_C_COMPILER} with Hazard3 RV32 flags")
    endif()
endif()

# Toolchain selection MUST happen before project(). --------------------

# The bare-metal toolchains have no libc/crt0, so CMake's test
# executable link would fail; compile-only checks are enough here.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(PLATFORM STREQUAL "esp32c3")
    # Cross-compile declaration keeps host flags out of the build.
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_SYSTEM_PROCESSOR riscv)
    find_program(CMAKE_C_COMPILER
        NAMES riscv32-esp-elf-gcc riscv64-unknown-elf-gcc riscv64-elf-gcc
        REQUIRED)
    find_program(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER} REQUIRED)
    set(ARCH_FLAGS "-march=rv32imc_zicsr" "-mabi=ilp32")
elseif(PLATFORM STREQUAL "esp32s3")
    # Xtensa LX7, call0 ABI (no register windows anywhere in the
    # kernel). The Espressif crosstool-NG release is expected under
    # ~/xtensa-tools/ (see docs/platforms.md); a triple on $PATH works
    # too. No call0 multilib libgcc exists, so the kernel carries its
    # own div/mod/shift helpers (arch/xtensa/runtime_helpers.c).
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_SYSTEM_PROCESSOR xtensa)
    find_program(CMAKE_C_COMPILER
        NAMES xtensa-esp32s3-elf-gcc
        PATHS "$ENV{HOME}/xtensa-tools/xtensa-esp32s3-elf/bin"
        REQUIRED)
    find_program(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER} REQUIRED)
    set(ARCH_FLAGS "-mabi=call0")
endif()

project(Scorpion C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -ffreestanding
    -Wall -Wextra -Wpedantic
)

if(NOT ARCH)
    if(PLATFORM STREQUAL "esp32s3")
        set(ARCH xtensa)
    else()
        set(ARCH riscv32)
    endif()
endif()

set(ARCH_DIR "${CMAKE_SOURCE_DIR}/arch/${ARCH}")

set(KERNEL_SOURCES
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
    abi/scorpion.h
)

if(PLATFORM STREQUAL "rp2350")
    list(APPEND KERNEL_SOURCES
        platform/rp2350/uart.c
        platform/rp2350/timer.c
        platform/rp2350/smp.c
        platform/rp2350/flash.c
        platform/rp2350/pmp.c
        platform/rp2350/payload.c
        ${ARCH_DIR}/context.S
        ${ARCH_DIR}/trap.S
        ${ARCH_DIR}/crt0.S
    )
    set(LINKER_SCRIPT "${ARCH_DIR}/kernel.ld")
    add_compile_definitions(PLATFORM_RP2350=1)
elseif(PLATFORM STREQUAL "esp32c3")
    list(APPEND KERNEL_SOURCES
        platform/esp32c3/uart.c
        platform/esp32c3/timer.c
        platform/esp32c3/smp_stub.c
        platform/esp32c3/flash.c
        platform/esp32c3/pmp_stub.c
        platform/esp32c3/atomic_compat.c
        ${ARCH_DIR}/context.S
        ${ARCH_DIR}/trap.S
        ${ARCH_DIR}/crt0.S
    )
    set(LINKER_SCRIPT "${ARCH_DIR}/kernel_esp32c3.ld")
    add_compile_definitions(PLATFORM_ESP32C3=1)
    add_compile_options(${ARCH_FLAGS})
elseif(PLATFORM STREQUAL "esp32s3")
    list(APPEND KERNEL_SOURCES
        platform/esp32s3/uart.c
        platform/esp32s3/timer.c
        platform/esp32s3/smp_stub.c
        platform/esp32s3/flash.c
        platform/esp32s3/pmp_stub.c
        ${ARCH_DIR}/context.S
        ${ARCH_DIR}/vectors.S
        ${ARCH_DIR}/crt0.S
        ${ARCH_DIR}/trap.c
        ${ARCH_DIR}/runtime_helpers.c
    )
    set(LINKER_SCRIPT "${ARCH_DIR}/kernel_esp32s3.ld")
    add_compile_definitions(PLATFORM_ESP32S3=1)
    add_compile_options(${ARCH_FLAGS})
else()
    message(FATAL_ERROR "Unknown PLATFORM '${PLATFORM}' "
                        "(use rp2350, esp32c3 or esp32s3)")
endif()

# Per-platform linker: riscv triples for the RV32 targets, the xtensa
# one for the S3.
if(PLATFORM STREQUAL "esp32s3")
    find_program(SCORPION_LD
        NAMES xtensa-esp32s3-elf-ld
        PATHS "$ENV{HOME}/xtensa-tools/xtensa-esp32s3-elf/bin"
        REQUIRED)
else()
    find_program(SCORPION_LD
        NAMES riscv64-elf-ld riscv64-unknown-elf-ld riscv32-esp-elf-ld
        REQUIRED)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Link with ld directly: some recent binutils builds crash inside the
# gcc driver's collect2 path for ELF32 RISC-V. libgcc is appended
# manually for the RISC-V targets (the rv32imc build needs its atomic/
# popcount/clz helpers); the Xtensa build has no call0 multilib and
# links its own runtime_helpers.c instead, so no libgcc there.
if(PLATFORM STREQUAL "esp32s3")
    set(LIBGCC_PATH "")
else()
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} ${ARCH_FLAGS} -print-libgcc-file-name
        OUTPUT_VARIABLE LIBGCC_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
set(CMAKE_C_LINK_EXECUTABLE
    "${SCORPION_LD} <LINK_FLAGS> <OBJECTS> ${LIBGCC_PATH} -o <TARGET>")

# --- RV32 user payloads (test SEF / controller SEF) ---
# These are hand-written RV32 assembly payloads; they are meaningless on
# the Xtensa S3 (no SEF dynamic linking there yet), so the whole block
# is gated off for that platform.
if(NOT PLATFORM STREQUAL "esp32s3")

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
    COMMAND ${SCORPION_LD} -m elf32lriscv -e _start --no-relax
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
    COMMAND ${SCORPION_LD} -m elf32lriscv -e _start --no-relax
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
add_custom_target(controller_sef_target
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/controller.sef)

endif() # NOT esp32s3

add_executable(WEW_scorpion ${KERNEL_SOURCES})

if(NOT PLATFORM STREQUAL "esp32s3")
    add_dependencies(WEW_scorpion controller_sef_target)
endif()

target_include_directories(WEW_scorpion PRIVATE "${CMAKE_SOURCE_DIR}")

if(PLATFORM STREQUAL "esp32s3")
    # xtensa ld infers the emulation from the objects; no -m needed.
    target_link_options(WEW_scorpion PRIVATE
        -T ${LINKER_SCRIPT}
        -Map=scorpion.map
    )
else()
    target_link_options(WEW_scorpion PRIVATE
        -m elf32lriscv
        -T ${LINKER_SCRIPT}
        -Map=scorpion.map
    )
endif()

add_custom_command(TARGET WEW_scorpion POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:WEW_scorpion> $<TARGET_FILE_DIR:WEW_scorpion>/WEW_scorpion.bin
)

if(PLATFORM STREQUAL "rp2350")
    add_custom_command(TARGET WEW_scorpion POST_BUILD
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/packrom.py
            $<TARGET_FILE_DIR:WEW_scorpion>/WEW_scorpion.bin
            ${CMAKE_CURRENT_BINARY_DIR}/controller.sef
            $<TARGET_FILE_DIR:WEW_scorpion>/Scorpion.uf2
        COMMENT "Packaged Scorpion.uf2 (boot2+kernel+controller)"
    )
elseif(PLATFORM STREQUAL "esp32c3")
    find_program(ESPTOOL_PY esptool.py esptool)
    add_custom_command(TARGET WEW_scorpion POST_BUILD
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/mkimage_esp32c3.py --chip c3
            $<TARGET_FILE:WEW_scorpion>
            $<TARGET_FILE_DIR:WEW_scorpion>/Scorpion_esp32c3.bin
        COMMENT "Packaged Scorpion_esp32c3.bin (ESP image, flash at 0x0)"
        VERBATIM
    )
elseif(PLATFORM STREQUAL "esp32s3")
    add_custom_command(TARGET WEW_scorpion POST_BUILD
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/mkimage_esp32c3.py --chip s3
            $<TARGET_FILE:WEW_scorpion>
            $<TARGET_FILE_DIR:WEW_scorpion>/Scorpion_esp32s3.bin
        COMMENT "Packaged Scorpion_esp32s3.bin (ESP image, flash at 0x0)"
        VERBATIM
    )
endif()
