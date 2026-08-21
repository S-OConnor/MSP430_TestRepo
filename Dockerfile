# Development image: TI msp430-gcc cross toolchain + CMake + flashing tools.
#
#   docker build -t msp430-dev .
#   docker run --rm -it -v "$PWD":/work msp430-dev
#   # inside:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/msp430-toolchain.cmake -G Ninja
#   cmake --build build
#
# Flashing from inside the container needs the LaunchPad passed through:
#   docker run --rm -it -v "$PWD":/work \
#       --device=/dev/bus/usb --device=/dev/ttyACM0 \
#       msp430-dev

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl bzip2 unzip \
        make cmake ninja-build \
        mspdebug \
    && rm -rf /var/lib/apt/lists/*

ARG MSP430_GCC_VER=9.3.1.11
ARG MSP430_GCC_REL=9.3.1.2
ARG MSP430_SUPPORT_VER=1.212
ARG TI_DL=https://dr-download.ti.com/software-development/ide-configuration-compiler-or-debugger/MD-LlCjWuAbzH

RUN mkdir -p /opt/ti && cd /opt/ti \
    && curl -fsSL -o gcc.tar.bz2 \
        ${TI_DL}/${MSP430_GCC_REL}/msp430-gcc-${MSP430_GCC_VER}_linux64.tar.bz2 \
    && tar xjf gcc.tar.bz2 && rm gcc.tar.bz2 \
    && curl -fsSL -o support.zip \
        ${TI_DL}/${MSP430_GCC_REL}/msp430-gcc-support-files-${MSP430_SUPPORT_VER}.zip \
    && unzip -q support.zip && rm support.zip \
    && ln -s msp430-gcc-${MSP430_GCC_VER}_linux64 msp430-gcc

ENV MSP430_GCC_ROOT=/opt/ti/msp430-gcc \
    MSP430_SUPPORT=/opt/ti/msp430-gcc-support-files/include \
    PATH=/opt/ti/msp430-gcc/bin:${PATH}

WORKDIR /work
CMD ["bash"]
