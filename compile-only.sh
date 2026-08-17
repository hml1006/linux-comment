#!/bin/bash

make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j12
# ./scripts/clang-tools/gen_compile_commands.py
