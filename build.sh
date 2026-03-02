cp arch/arm64/configs/defconfig .config
sed -i 's/=m/=y/g' .config
sed -i 's/^# CONFIG_GDB_SCRIPTS is not set/CONFIG_GDB_SCRIPTS=y/' .config
sed -i 's/^CONFIG_DEBUG_INFO_REDUCED=y/CONFIG_DEBUG_INFO_REDUCED=n/' .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j12
./scripts/clang-tools/gen_compile_commands.py
