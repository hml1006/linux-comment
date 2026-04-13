## nvme是什么
NVMe是一种Host与SSD之间通讯的协议, 它在协议栈中隶属高层, 类比tcp/ip协议.

## nvme和pcie总线关系
nvme over pcie, nvme协议使用pcie bar0和bar1两个寄存器表示一个64位地址,指示nvme协议寄存器地址.
ioremap之后, nvme协议的管理控制类命令执行过程通过主机内存访问方式, admin和io队列中的命令通过dma方式传递给nvme controller, nvme设备获取数据通过命令中的dma地址执行dma操作.

## nvme over fabric
nvme协议还有RDMA/TCP等载体.
