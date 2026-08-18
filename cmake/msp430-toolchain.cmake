# Cross-compilation toolchain for TI msp430-gcc (MSP430-GCC-OPENSOURCE).
#
# Locations (first match wins):
#   -DMSP430_GCC_ROOT=... / -DMSP430_SUPPORT=...   on the cmake command line
#   MSP430_GCC_ROOT / MSP430_SUPPORT               environment variables
#   $HOME/ti/msp430-gcc, $HOME/ti/msp430-gcc-support-files/include (defaults)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR msp430)

if(NOT DEFINED MSP430_GCC_ROOT)
    if(DEFINED ENV{MSP430_GCC_ROOT})
        set(MSP430_GCC_ROOT "$ENV{MSP430_GCC_ROOT}")
    else()
        set(MSP430_GCC_ROOT "$ENV{HOME}/ti/msp430-gcc")
    endif()
endif()
set(MSP430_GCC_ROOT "${MSP430_GCC_ROOT}" CACHE PATH "TI msp430-gcc install root")

if(NOT DEFINED MSP430_SUPPORT)
    if(DEFINED ENV{MSP430_SUPPORT})
        set(MSP430_SUPPORT "$ENV{MSP430_SUPPORT}")
    else()
        set(MSP430_SUPPORT "$ENV{HOME}/ti/msp430-gcc-support-files/include")
    endif()
endif()
set(MSP430_SUPPORT "${MSP430_SUPPORT}" CACHE PATH
    "TI support-files include dir (device headers + linker scripts)")

set(CMAKE_C_COMPILER   "${MSP430_GCC_ROOT}/bin/msp430-elf-gcc")
set(CMAKE_ASM_COMPILER "${MSP430_GCC_ROOT}/bin/msp430-elf-gcc")
set(CMAKE_OBJCOPY      "${MSP430_GCC_ROOT}/bin/msp430-elf-objcopy" CACHE FILEPATH "")
set(CMAKE_SIZE         "${MSP430_GCC_ROOT}/bin/msp430-elf-size"    CACHE FILEPATH "")

# The compile test can't link a host executable; build a static lib instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${MSP430_GCC_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
