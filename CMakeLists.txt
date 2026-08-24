cmake_minimum_required(VERSION 3.16)

# Configure with the cross toolchain file:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/msp430-toolchain.cmake
project(adc168m102_fw C)

set(MCU msp430fr5969)

add_executable(fw.elf
    src/main.c
    src/clocks.c
    src/spi.c
    src/uart.c
    src/adc168m102.c
)

set_target_properties(fw.elf PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)

target_include_directories(fw.elf PRIVATE src "${MSP430_SUPPORT}")

target_compile_options(fw.elf PRIVATE
    -mmcu=${MCU}
    -mhwmult=f5series
    -Os -g3
    -Wall -Wextra
    -ffunction-sections -fdata-sections
)

target_link_options(fw.elf PRIVATE
    -mmcu=${MCU}
    "-L${MSP430_SUPPORT}"
    -Wl,--gc-sections
    "-Wl,-Map=$<TARGET_FILE_DIR:fw.elf>/fw.map"
)

add_custom_command(TARGET fw.elf POST_BUILD
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:fw.elf>"
    COMMENT "Section sizes"
)

# --- convenience targets --------------------------------------------------
# `tilib` needs TI's libmsp430.so (MSP Debug Stack / CCS / UniFlash) on
# LD_LIBRARY_PATH; `ezfet` is mspdebug's driver-free eZ-FET fallback.
add_custom_target(flash
    COMMAND mspdebug tilib "prog $<TARGET_FILE:fw.elf>"
    DEPENDS fw.elf
    USES_TERMINAL
)

add_custom_target(flash-ezfet
    COMMAND mspdebug ezfet "prog $<TARGET_FILE:fw.elf>"
    DEPENDS fw.elf
    USES_TERMINAL
)
