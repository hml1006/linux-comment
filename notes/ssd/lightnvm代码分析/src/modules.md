## 模块简介
代码流路径: linux/drivers/lightnvm/

### *lnvm模块*
模块入口: nvme-core.c -> module_init(nvme_init);
模块主要功能: pcie驱动+nvme命令实现

### *nvm_mgr*
模块入口: nvm-core.c ->module_init(nvm_mod_init);
模块主要功能: 提供用户空间管理nvme设备的接口

### *venice*
模块入口: venice_module.c ->module_init(venice_init);
模块主要功能: 

### 模块插入顺序

```bash
root@louis:code/linux-venice/drivers/lightnvm# insmod nvm_mgr.ko 
root@louis:code/linux-venice/drivers/lightnvm# insmod lnvm.ko 
root@louis:code/linux-venice/drivers/lightnvm# insmod venice.ko 
```
