set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR hazard3)

set(TOOLCHAIN_PREFIX riscv32-unknown-elf)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}-objcopy)
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}-objdump)
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}-size)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ARCH riscv32)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32 -mfloat-abi=softfp -ffreestanding -nostdlib")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32")

set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/arch/${ARCH}/kernel.ld")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T ${LINKER_SCRIPT} -nostartfiles -nostdlib")
