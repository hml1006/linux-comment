OCS盘开卡后，是裸盘状态，没有块设备，需要用ocnvme工具（GIT: 172.17.9.216/common/nvme-cli.git, branch: smi_1.7）创建块设备。

ocnvme工具依赖liblightnvm（GIT: 172.17.9.216/common/liblightnvm, branch: smi），需要make install

创建块设备：ocnvme lnvm create -c 3840GB -d lnvm0n1 [-n osa]
删除块设备：ocnvme lnvm remove -n osa

其他常用的命令：
```
	ocnvme list
	ocnvme format /dev/osa
	ocnvme smart-log osa
	ocnvme lnvm status osa
	ocnvme lnvm smart-log-add osa
```
