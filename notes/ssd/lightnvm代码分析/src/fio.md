fio为io压测工具

### jobfile参数
```
# write iops

[global]
ioengine=libaio
direct=1
norandommap
randrepeat=0
group_reporting
ignore_error=EIO,EIO
#write_bw_log=write_iops
#write_iops_log=write_iops
#write_lat_log=write_iops
#log_avg_msec=1000
scramble_buffers=0
rwmixread=70
size=1%
# 3840g is 3576g in real
#io_size=3576g
filename=/dev/osa
runtime=36000
time_based

[write_iops]
gtod_reduce=1
rw=randrw
bs=4k
numjobs=4
iodepth=256
iodepth_batch=64
iodepth_batch_complete=64
```
### 命令
fio命令后跟配置文件路径
```
fio jobfile
```
