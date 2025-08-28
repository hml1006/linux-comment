epilog数据分epilog head和表项，表项为metadata，高4位固定 0xf, 低36位为lba地址，中间保留部分为namespace，暂未用到  
每个super block的epilog表映射的都是这个super block自身的地址