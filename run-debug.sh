#!/bin/bash -x

if [ ! -f "nvme.img" ];then
  echo "Create a nvme SSD"
  dd if=/dev/zero of=nvme.img bs=1M count=512
fi

if [ ! -e "k_shared" ];then
  echo "Create folder k_shared"
  mkdir k_shared
elif [ ! -d "k_shared" ];then
  echo "k_shared not a folder, delete it"
  rm -f k_shared
  echo "Create folder k_shared"
  mkdir k_shared
fi

cur_dir=`pwd`
gdb_cfg=${HOME}/.config/gdb
mkdir -p ${gdb_cfg}
echo "add-auto-load-safe-path ${cur_dir}/.gdbinit" > ${gdb_cfg}/gdbinit
echo "set auto-load python-scripts on" > .gdbinit
echo "add-auto-load-safe-path ${cur_dir}" >> .gdbinit

echo "set architecture aarch64" >> .gdbinit
echo "file vmlinux" >> .gdbinit
echo "b start_kernel" >> .gdbinit

br0_exists=`ifconfig | grep br0 | wc -l`
if [ ${br0_exists} -eq 1 ];then
  sudo brctl delbr br0 # 删除网桥设备 br0
fi

#获取当前使用中的网卡
nic_list=`ls /sys/class/net/`
for nic in ${nic_list}
do
  if [[ ${nic} == "lo" && ${nic} == "br0" && ${nic} == "tun0" && ${nic} == "tap0" ]];then
    break
  fi
  rx_bytes=`cat /sys/class/net/${nic}/statistics/rx_bytes`
  tx_bytes=`cat /sys/class/net/${nic}/statistics/tx_bytes`
  if [[ ${rx_bytes} -gt 0 && ${tx_bytes} -gt 0 ]];then
    export used_nic=${nic}
    break
  fi
done

# wifi网络无法加入网桥，所以只能用nat方式
sudo apt-get install uml-utilities -y # 安装 tun/tap 工具箱
echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward

sudo tunctl -t tap0 -u root # 建立 tap0
sudo ifconfig tap0 192.168.5.2 promisc up # 将 tap0 设置为混杂模式
sudo route add -net 192.168.5.0 netmask 255.255.255.0 dev tap0
sudo iptables -t nat -A POSTROUTING -s 192.168.5.0/24 -o ${used_nic} -j MASQUERADE

# 修改ctr c
stty intr ^]

sudo qemu-system-aarch64 -cpu cortex-a710 -machine virt \
  -m 4096 -smp 2 \
  -net nic -net tap,ifname=tap0,script=no,downscript=no \
  -drive file=nvme.img,format=raw,if=none,id=nvm0 \
  -device nvme,serial=12345678,drive=nvm0 \
  -kernel arch/arm64/boot/Image \
  --fsdev local,id=kmod_dev,path=$PWD/k_shared,security_model=none \
  -device virtio-9p-device,fsdev=kmod_dev,mount_tag=kmod_mount \
  --append "root=/dev/vda rootfstype=ext4 rw loglevel=8 console=ttyAMA0 nokaslr"  \
  -serial stdio \
  -drive file=ubuntu-rootfs.img,index=0,media=disk,format=raw -S -s

# 恢复ctr c
stty intr ^c
