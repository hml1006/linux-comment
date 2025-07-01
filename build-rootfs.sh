#!/bin/bash

# 创建Ubuntu ARM64 rootfs 方法
# https://qubot.org/2023/08/09/h618-%E7%A7%BB%E6%A4%8Dubuntu-22-04-rootfs/

sudo apt-get install qemu-user-static
if [ ! -e ubuntu-base-25.04-base-arm64.tar.gz ];then
    wget http://cdimage.ubuntu.com/ubuntu-base/releases/25.04/release/ubuntu-base-25.04-base-arm64.tar.gz
fi
if [ ! -e rootfs ];then
    mkdir rootfs
    tar -xzvf ubuntu-base-25.04-base-arm64.tar.gz -C rootfs
fi

ca_certificates=ca-certificates_20250419_all.deb
if [ ! -e ${ca_certificates} ];then
	wget http://ports.ubuntu.com/pool/main/c/ca-certificates/${ca_certificates}
fi

openssl=openssl_3.5.0-2ubuntu1_arm64.deb
if [ ! -e ${openssl} ];then
	wget http://ports.ubuntu.com/pool/main/o/openssl/${openssl}
fi

libssl=libssl3t64_3.5.0-2ubuntu1_arm64.deb
if [ ! -e ${libssl} ];then
	wget http://ports.ubuntu.com/pool/main/o/openssl/${libssl}
fi 

sudo cp ${ca_certificates} ./rootfs/root/
sudo cp ${openssl} ./rootfs/root/
sudo cp ${libssl} ./rootfs/root/

sudo rm -f ./rootfs/etc/resolv.conf
resolv=$(cat <<"EOF"
nameserver 8.8.8.8
options edns0 trust-ad
search localdomain
EOF
)
sudo echo "${resolv}" > ./rootfs/etc/resolv.conf

interfaces=$(cat <<"EOF"
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
address 192.168.5.80
netmask 255.255.255.0
gateway 192.168.5.2
EOF
)
mkdir -p ./rootfs/etc/network/
sudo echo "${interfaces}" > ./rootfs/etc/network/interfaces

source_list=$(cat <<"EOF"
# 默认注释了源码镜像以提高 apt update 速度，如有需要可自行取消注释
deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky main restricted universe multiverse
# deb-src https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky main restricted universe multiverse
deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky-updates main restricted universe multiverse
# deb-src https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky-updates main restricted universe multiverse
deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky-backports main restricted universe multiverse
# deb-src https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky-backports main restricted universe multiverse

# 以下安全更新软件源包含了官方源与镜像站配置，如有需要可自行修改注释切换
deb http://ports.ubuntu.com/ubuntu-ports/ plucky-security main restricted universe multiverse
# deb-src http://ports.ubuntu.com/ubuntu-ports/ plucky-security main restricted universe multiverse

# 预发布软件源，不建议启用
# deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky-proposed main restricted universe multiverse
# # deb-src https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ plucky-proposed main restricted universe multiverse
EOF
)
sudo echo "${source_list}" > ./rootfs/etc/apt/sources.list

install=$(cat <<"EOF"
#!/bin/sh
cd /root/
useradd -G sudo -m -s /bin/bash louis
echo louis:yes | chpasswd
passwd root
echo louis.arm > /etc/hostname
echo 127.0.0.1	localhost > /etc/hosts
chmod 777 /tmp/
dpkg -i *.deb
apt update
apt upgrade -y
apt install -y dialog perl-base systemd sudo vim kmod net-tools ethtool ifupdown rsyslog htop iputils-ping language-pack-en-base ssh iputils-ping resolvconf wget
ln -s /lib/systemd/system/getty\@.service /etc/systemd/system/getty.target.wants/getty\@ttyAMA0.service
EOF
)

sudo rm -f ./rootfs/etc/systemd/resolved.conf
resolved=$(cat <<"EOF"
[Resolve]
DNS=8.8.8.8
DNSStubListener=no
EOF
)
sudo echo "${install}" > ./rootfs/root/install.sh
sudo echo "${resolv}" > ./rootfs/etc/resolv.conf
sudo echo "${resolved}" > ./rootfs/etc/systemd/resolved.conf

sudo chmod +x ./rootfs/root/install.sh
sudo cp /usr/bin/qemu-arm-static ./rootfs/usr/bin/
sudo mount -t proc /proc ./rootfs/proc
sudo mount -t sysfs /sys ./rootfs/sys
sudo mount -o bind /dev ./rootfs/dev
sudo mount -o bind /dev/pts ./rootfs/dev/pts
sudo chroot ${PWD}/rootfs /root/install.sh
sudo umount ./rootfs/proc
sudo umount ./rootfs/sys
sudo umount ./rootfs/dev/pts
sudo umount ./rootfs/dev/

dd if=/dev/zero of=ubuntu-rootfs.img bs=1M count=2048
sudo mkfs.ext4  ubuntu-rootfs.img
rm -rf ubuntu-mount
mkdir ubuntu-mount
sudo mount ubuntu-rootfs.img ubuntu-mount/
sudo cp -rfp rootfs/*  ubuntu-mount/
sudo umount ubuntu-mount/
sudo rm -rf ubuntu-mount/
e2fsck -p -f ubuntu-rootfs.img
