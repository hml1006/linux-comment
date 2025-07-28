**中断向量表vectors**

```
SYM_CODE_START(vectors)
// el1t， linux未处理，正常不会进入
    kernel_ventry    1, t, 64, sync        // Synchronous EL1t
    kernel_ventry    1, t, 64, irq        // IRQ EL1t
    kernel_ventry    1, t, 64, fiq        // FIQ EL1t
    kernel_ventry    1, t, 64, error        // Error EL1t
    
// 硬件中断，内核内存访问异常等
    kernel_ventry    1, h, 64, sync        // Synchronous EL1h
    kernel_ventry    1, h, 64, irq        // IRQ EL1h
    kernel_ventry    1, h, 64, fiq        // FIQ EL1h
    kernel_ventry    1, h, 64, error        // Error EL1h

// 系统调用，用户空间内存访问异常等
    kernel_ventry    0, t, 64, sync        // Synchronous 64-bit EL0
    kernel_ventry    0, t, 64, irq        // IRQ 64-bit EL0
    kernel_ventry    0, t, 64, fiq        // FIQ 64-bit EL0
    kernel_ventry    0, t, 64, error        // Error 64-bit EL0

// 32位程序系统调用，用户空间内存访问异常等
    kernel_ventry    0, t, 32, sync        // Synchronous 32-bit EL0
    kernel_ventry    0, t, 32, irq        // IRQ 32-bit EL0
    kernel_ventry    0, t, 32, fiq        // FIQ 32-bit EL0
    kernel_ventry    0, t, 32, error        // Error 32-bit EL0
SYM_CODE_END(vectors)
```

为了缓解间接分支预测漏洞造成的cache泄露给user space，可以启用tramp_vectors，通过tramp_vectors跳转到vectors，这个patch会降低性能，所以是个可选项。

cpu间接分支预测会提前执行指令，如果用户空间指令访问了内核空间地址，会把数据从内存加载到cache，虽然会访问失败，但是通过一些特殊操作可以在用户空间访问到内核泄露的cache数据。

tramp_vectors是为了缓解这个问题，当cpu位于el0，内核页表寄存器指向tramp_pg_dir，这个页表比swapper_pg_dir小很多，只指向了一些处理用户空间操作的必要页面。当执行系统调用时，先跳转到tramp_vector代码，恢复内核页表和中断向量寄存器为swapper_pg_dir和vectors，然后再正常处理系统调用。当返回用户空间时，再重新恢复到tramp_vectors和tramp_pg_dir。

这就是KPTI(内核页表隔离), 这个功能开启会对性能造成较大影响。

**tramp_vectors**

如果开启历史分支预测幽灵漏洞缓解，会有4张表。

```
SYM_CODE_START_NOALIGN(tramp_vectors)
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
    generate_tramp_vector    kpti=1, bhb=BHB_MITIGATION_LOOP
    generate_tramp_vector    kpti=1, bhb=BHB_MITIGATION_FW
    generate_tramp_vector    kpti=1, bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
    generate_tramp_vector    kpti=1, bhb=BHB_MITIGATION_NONE
SYM_CODE_END(tramp_vectors)
```

一张表的结构是：

```
    .macro    generate_tramp_vector,    kpti, bhb
.Lvector_start\@:
    .space    0x400                    // 空出8个entry, 这段属于el1t, el1h, 只有el0t, el0t 32才需要,也就是el0切el1才需要tramp_vector

    .rept    4                        // 此处是el0t的4个entry, repeat 4次
    tramp_ventry    .Lvector_start\@, 64, \kpti, \bhb
    .endr
    .rept    4                        // 此处是el0t_32的4个entry
    tramp_ventry    .Lvector_start\@, 32, \kpti, \bhb
    .endr
    .endm
```

> 因为要缓解的是el0访问el1数据问题，所以只实现了el0t, el0t_32.

**tramp_ventry执行流程**

```
tramp_ventry
    -->tramp_map_kernel
        -->恢复ttbr1_el1页表寄存器，tramp_pg_dir->swapper_pg_dir
    -->tramp_data_read_var<取vectors中断向量表地址>
    -->恢复vbar_el1寄存器为vectors
    -->修改x30寄存器地址, 指向对应vectors entry的第二条指令地址
    -->ret返回到vectors的entry
```

**kernel_ventry执行流程**

```
kernel_ventry
    -->从tpidrro_el0恢复x30
    -->预留备份寄存器的栈空间
    -->检查sp是否溢出<vmap stack>
    -->跳转到entry_handler宏执行
```

**entry_handler执行流程**

entry_handler是个macro，代码中定义了 el\el\ht\()_\regsize\()_\label 这些函数，kernel_ventry就是跳转到这几个函数。

```
    .macro entry_handler el:req, ht:req, regsize:req, label:req
SYM_CODE_START_LOCAL(el\el\ht\()_\regsize\()_\label)        // kernel_ventry会跳转到此处执行
    kernel_entry \el, \regsize                                // 备份通用寄存器, 部分初始化操作
    mov    x0, sp
    bl    el\el\ht\()_\regsize\()_\label\()_handler            // 跳转到handler执行
    .if \el == 0    // 表示系统调用或者用户态触发的异常
    b    ret_to_user
    .else
    b    ret_to_kernel
    .endif
SYM_CODE_END(el\el\ht\()_\regsize\()_\label)
    .endm
```

> entry_handler先执行kernel_entry, kernel_entry主要功能是保存通用寄存器和切堆栈，然后是MTE和ptr auth设置。之后跳转到c代码el\el\ht()_\regsize()_\label()_handler
