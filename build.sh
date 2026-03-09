#cp arch/arm64/configs/defconfig .config

sed -i 's/^# CONFIG_GDB_SCRIPTS is not set/CONFIG_GDB_SCRIPTS=y/' .config
sed -i 's/^# CONFIG_DYNAMIC_DEBUG is not set/CONFIG_DYNAMIC_DEBUG=y/' .config
sed -i 's/^# CONFIG_DYNAMIC_DEBUG_CORE is not set/CONFIG_DYNAMIC_DEBUG_CORE=y/' .config
sed -i 's/^CONFIG_DEBUG_INFO_REDUCED=y/CONFIG_DEBUG_INFO_REDUCED=n/' .config

# 检查CONFIG_GDB_SCRIPTS
CONFIG_GDB_SCRIPTS_cnt=`cat .config | grep CONFIG_GDB_SCRIPTS | wc -l`
if [ "$CONFIG_GDB_SCRIPTS_cnt" -eq "0" ]; then
    echo "CONFIG_GDB_SCRIPTS=y" >> .config
fi

# 开启pr debug
CONFIG_DYNAMIC_DEBUG_cnt=`cat .config | grep CONFIG_DYNAMIC_DEBUG | wc -l`
if [ "$CONFIG_DYNAMIC_DEBUG_cnt" -eq "0" ]; then
    echo "CONFIG_DYNAMIC_DEBUG=y" >> .config
fi
CONFIG_DYNAMIC_DEBUG_CORE_cnt=`cat .config | grep CONFIG_DYNAMIC_DEBUG_CORE | wc -l`
if [ "$CONFIG_DYNAMIC_DEBUG_CORE_cnt" -eq "0" ]; then
    echo "CONFIG_DYNAMIC_DEBUG_CORE=y" >> .config
fi

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig

sed -i 's/=m/=y/g' .config

make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j12
./scripts/clang-tools/gen_compile_commands.py
