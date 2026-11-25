# 块层 — 设备管理与调试 (Part III)

> 本文档拆分自 [block_layer_analysis.md](block_layer_analysis.md) Part III，涵盖分区管理、数据完整性与加密、Zoned块设备、Sysfs与调试接口、超时与电源管理、统计与跟踪

## Part III: 设备管理与调试

## 11. 分区管理

### 11.1 分区核心（block/partitions/core.c, 732 行）

文件：`block/partitions/core.c`

负责分区表的检测和解析，通过 `check_part[]` 函数指针数组按优先级顺序尝试各种分区格式：

- 优先检测磁盘地址 0 处的分区表格式（有 ADFS 引导块）
- 然后检测磁盘地址 0xDC0 处的分区信息
- 内核命令行分区（`cmdline_partition`）
- 设备树分区（`of_partition`）
- EFI/GPT 分区

### 11.2 支持的分区格式

| 文件 | 行数 | 分区格式 |
|------|------|----------|
| msdos.c | 717 | DOS/MBR 分区表 |
| efi.c | 756 | EFI/GPT 分区表 |
| ldm.c | 1,487 | Windows 动态磁盘（LDM） |
| acorn.c | 550 | Acorn 分区 |
| cmdline.c | 385 | 内核命令行分区 |
| ibm.c | 414 | IBM S/390 分区 |
| aix.c | 282 | AIX 分区 |
| mac.c | — | Apple Macintosh 分区 |
| sun.c | — | Sun Solaris 分区 |
| sgi.c | — | SGI 分区 |
| atari.c | — | Atari 分区 |
| amiga.c | — | Amiga 分区 |
| osf.c | — | OSF/Unix 分区 |
| karma.c | — | Karma 分区 |
| sysv68.c | — | SysV 68 分区 |
| ultrix.c | — | Ultrix 分区 |
| of.c | — | Open Firmware 分区 |

### 11.3 DOS/MBR 分区表详解

文件：`block/partitions/msdos.c`（717 行）

MBR（Master Boot Record）是 IBM PC 兼容机最早使用的分区表格式，位于磁盘的 0 号扇区（LBA 0）。Linux 内核通过 `msdos_partition()` 函数解析 MBR 分区表。

#### 11.3.1 MBR 扇区布局

MBR 扇区（512 字节）的完整布局：

```
偏移量    大小    内容
─────────────────────────────────────────────────
0x000     440     引导代码（boot code）
0x1B8       4     磁盘签名（disk signature, unique MBR signature）
0x1BC       2     未知（通常为 0x0000）
0x1BE      64     分区表（4 个主分区表项，每个 16 字节）
0x1FE       2     签名（0x55 0xAA）
```

- **磁盘签名**（`unique_mbr_signature`）：位于偏移 0x1B8 的 4 字节值，用于唯一标识磁盘，通过 `disksig` 传递给 `set_info()` 生成分区 UUID。
- **分区表**：紧接在偏移 0x1BE 处，共 64 字节，包含 4 个主分区表项。
- **签名**：最后两个字节固定为 `0x55AA`，用于标识这是一个有效的 MBR。

#### 11.3.2 核心数据结构

**MBR 分区表项**（[msdos_partition.h](file:///home/louis/code/linux/include/linux/msdos_partition.h)）：

```c
struct msdos_partition {
    u8     boot_ind;      // 引导标志（0x80=可引导, 0x00=不可引导）
    u8     head;          // 起始磁头（CHS 寻址）
    u8     sector;        // 起始扇区（低 6 位, 高 2 位在 cyl 中）
    u8     cyl;           // 起始柱面（高位与 sector 共用）
    u8     sys_ind;       // 分区类型标识（见枚举 msdos_sys_ind）
    u8     end_head;      // 结束磁头
    u8     end_sector;    // 结束扇区
    u8     end_cyl;       // 结束柱面
    __le32 start_sect;    // 起始 LBA（4 字节, 相对于磁盘起始）
    __le32 nr_sects;      // 分区大小（扇区数, 4 字节）
} __packed;  // 共 16 字节
```

**分区类型标识**（`sys_ind` 枚举值）：

```c
enum msdos_sys_ind {
    DOS_EXTENDED_PARTITION      = 5,     // DOS 扩展分区
    WIN98_EXTENDED_PARTITION    = 0x0f,  // Windows 98 扩展分区（LBA）
    LINUX_EXTENDED_PARTITION    = 0x85,  // Linux 扩展分区
    LINUX_DATA_PARTITION        = 0x83,  // Linux 数据分区（ext2/3/4, XFS 等）
    LINUX_SWAP_PARTITION        = 0x82,  // Linux swap 分区（也用于 Solaris）
    LINUX_LVM_PARTITION         = 0x8e,  // Linux LVM 分区
    LINUX_RAID_PARTITION        = 0xfd,  // Linux RAID 自动检测分区
    FREEBSD_PARTITION           = 0xa5,  // FreeBSD 分区
    OPENBSD_PARTITION           = 0xa6,  // OpenBSD 分区
    NETBSD_PARTITION            = 0xa9,  // NetBSD 分区
    UNIXWARE_PARTITION          = 0x63,  // Unixware / SCO Unix / GNU HURD
    DM6_PARTITION               = 0x54,  // Disk Manager 6（有 DDO）
    EZD_PARTITION               = 0x55,  // EZ-DRIVE
};
```

**扩展分区判断**：

```c
static inline int is_extended_partition(struct msdos_partition *p)
{
    return (p->sys_ind == DOS_EXTENDED_PARTITION ||
            p->sys_ind == WIN98_EXTENDED_PARTITION ||
            p->sys_ind == LINUX_EXTENDED_PARTITION);
}
```

三种扩展分区类型（5, 0x0f, 0x85）行为完全相同，区别在于：
- `DOS_EXTENDED_PARTITION (5)`：传统 CHS 扩展分区
- `WIN98_EXTENDED_PARTITION (0x0f)`：支持 LBA 寻址的扩展分区（推荐）
- `LINUX_EXTENDED_PARTITION (0x85)`：Linux 专用扩展分区

#### 11.3.3 MBR 解析流程

`msdos_partition()` 的完整调用流程：

```text
msdos_partition(state)
  │  # 入口: 读取 LBA 0 扇区
  │
  ├─ 1. 读取 MBR 扇区
  │   read_part_sector(state, 0, &sect)
  │
  ├─ 2. AIX 魔数检测
  │   aix_magic_present(state, data)
  │   └─ 检查 0x1BE 处 partition 是否有 Linux 类型
  │       └─ 若无, 检查 LBA 7 的 "_LVM" 签名 → 若匹配则 return aix_partition()
  │
  ├─ 3. 签名检查
  │   msdos_magic_present(data + 510)  →  检查 0x55AA 签名
  │   └─ 不匹配则 return 0
  │
  ├─ 4. Boot indicator 有效性检查
  │   └─ 遍历 4 个分区表项, 检查 boot_ind 是否为 0 或 0x80
  │       └─ 若非法且不是 FAT 文件系统, 则 return 0
  │
  ├─ 5. GPT 保护性 MBR 检查（CONFIG_EFI_PARTITION）
  │   └─ 遍历 4 个分区表项, 若 sys_ind == 0xEE (EFI_PMBR_OSTYPE_EFI_GPT)
  │       └─ return 0（让 GPT 解析器处理）
  │
  ├─ 6. 读取磁盘签名
  │   disksig = le32_to_cpup(data + 0x1B8)
  │
  ├─ 7. 第一遍: 主分区与扩展分区
  │   state->next = 5  # 逻辑分区从 5 开始编号
  │   └─ for slot = 1..4:
  │       ├─ 若 size == 0: continue
  │       ├─ 若 is_extended_partition(p):
  │       │   ├─ put_partition(slot, start, n)  # 扩展分区占位
  │       │   └─ parse_extended(state, start, size, disksig)  # 解析逻辑分区
  │       └─ 否则:
  │           └─ put_partition(state, slot, start, size)
  │               set_info(state, slot, disksig)  # 设置分区 UUID
  │
  └─ 8. 第二遍: 子分区解析
      └─ for slot = 1..4:
          └─ subtypes[n].parse(state, start, size, slot)
              ├─ parse_freebsd → parse_bsd()  # BSD disklabel
              ├─ parse_netbsd  → parse_bsd()
              ├─ parse_openbsd → parse_bsd()
              ├─ parse_minix                    # Minix 子分区
              ├─ parse_unixware                 # Unixware 子分区
              └─ parse_solaris_x86              # Solaris VTOC
```

**MBR 解析中的关键辅助函数**：

```c
// 读取扇区（通过页缓存）
static inline sector_t nr_sects(struct msdos_partition *p)
{
    return (sector_t)get_unaligned_le32(&p->nr_sects);
}

static inline sector_t start_sect(struct msdos_partition *p)
{
    return (sector_t)get_unaligned_le32(&p->start_sect);
}

// 设置分区元信息（UUID）
static void set_info(struct parsed_partitions *state, int slot, u32 disksig)
{
    struct partition_meta_info *info = &state->parts[slot].info;
    snprintf(info->uuid, sizeof(info->uuid), "%08x-%02x", disksig, slot);
    info->volname[0] = 0;
    state->parts[slot].has_info = true;
}
```

#### 11.3.4 扩展分区与逻辑分区

MBR 只能记录 4 个主分区，若要支持更多分区，需要将其中一个主分区标记为扩展分区。扩展分区通过链式结构组织逻辑分区：

**扩展分区布局**：

```
磁盘布局:
┌──────────┬──────────┬──────────┬──────────┬──────────┐
│ 主分区1  │ 主分区2  │ 扩展分区  │ 主分区4  │          │
│ (/dev/sda1)│(/dev/sda2)│(container)│(/dev/sda4)│          │
└──────────┴──────────┴──────────┴──────────┴──────────┘
                          │
                          ▼
          扩展分区内部（链式 EBR）:
          ┌─────────────────┬─────────────────┐
          │ EBR 1           │ 逻辑分区 1       │
          │ [sda5] [→EBR 2] │ (/dev/sda5)      │
          ├─────────────────┼─────────────────┤
          │ EBR 2           │ 逻辑分区 2       │
          │ [sda6] [→EBR 3] │ (/dev/sda6)      │
          ├─────────────────┼─────────────────┤
          │ EBR 3           │ 逻辑分区 3       │
          │ [sda7] [→NULL]  │ (/dev/sda7)      │
          └─────────────────┴─────────────────┘
```

每个 EBR（Extended Boot Record）的结构与 MBR 完全相同：
- 偏移 0x1BE：第一个分区表项 → 指向当前逻辑分区的数据区域
- 偏移 0x1CE：第二个分区表项 → 指向下一个 EBR 的位置

**`parse_extended()` 解析逻辑**：

```c
static void parse_extended(struct parsed_partitions *state,
                           sector_t first_sector, sector_t first_size,
                           u32 disksig)
{
    int loopct = 0;   // 循环防护计数器（上限 100）
    this_sector = first_sector;  // 起始于扩展分区的第一个扇区

    while (1) {
        if (++loopct > 100) return;  // 防无限循环
        if (state->next == state->limit) return;  // 分区数上限

        data = read_part_sector(state, this_sector, &sect);
        if (!msdos_magic_present(data + 510)) goto done;

        p = (struct msdos_partition *)(data + 0x1be);

        // 第一遍: 处理数据分区（逻辑分区）
        for (i = 0; i < 4; i++, p++) {
            if (!nr_sects(p) || is_extended_partition(p)) continue;
            offs = start_sect(p) * sector_size;
            size = nr_sects(p) * sector_size;
            next = this_sector + offs;   // 逻辑分区的绝对起始地址
            put_partition(state, state->next, next, size);
            // ...
        }

        // 第二遍: 查找下一个扩展分区链接
        p -= 4;
        for (i = 0; i < 4; i++, p++)
            if (nr_sects(p) && is_extended_partition(p)) break;
        if (i == 4) goto done;  // 没有更多扩展分区链接

        // 移动到下一个 EBR
        this_sector = first_sector + start_sect(p) * sector_size;
    }
}
```

**关键设计要点**：
- 每个 EBR 读取后检查 `0x55AA` 签名
- `loopct` 上限 100 防止损坏的扩展分区表导致无限循环
- 逻辑分区编号从 5 开始（`state->next = 5`）
- 第 3、4 个 EBR 分区表项检查边界（不能超出扩展分区范围）

#### 11.3.5 MBR 的局限性

| 限制 | 说明 |
|------|------|
| 最大磁盘容量 | 2 TiB（因为 `start_sect` 和 `nr_sects` 均为 32 位） |
| 最大分区数 | 4 个主分区，或 3 个主分区 + 最多约 60 个逻辑分区 |
| 数据冗余 | 无备份分区表，MBR 损坏则整个磁盘不可访问 |
| UUID 支持 | 仅 4 字节磁盘签名，不支持真正的 GUID |
| 分区属性 | 仅有 1 字节类型标识，无属性标志位 |

### 11.4 EFI/GPT 分区表详解

文件：`block/partitions/efi.c`（756 行），头文件：`block/partitions/efi.h`

GPT（GUID Partition Table）是 UEFI 规范定义的新一代分区表格式，克服了 MBR 的 2TiB 限制和 4 个主分区限制。

#### 11.4.1 GPT 磁盘布局

```
LBA 0      ┌─────────────────────────────────────────┐
           │  保护性 MBR（PMBR）                      │
           │  - 分区类型 0xEE, 覆盖整个磁盘或 2TiB    │
           └─────────────────────────────────────────┘
LBA 1      ┌─────────────────────────────────────────┐
           │  GPT 头（Primary GPT Header）            │
           │  - 签名: "EFI PART"                      │
           │  - 分区表位置: LBA 2                     │
           │  - 备份 GPT 头位置: 磁盘最后一个 LBA     │
           └─────────────────────────────────────────┘
LBA 2      ┌─────────────────────────────────────────┐
           │  GPT 分区表项数组（Primary Partition Entries）│
           │  - 默认 128 个条目 × 128 字节 = 16KB     │
           │  - 通常占用 LBA 2~33（共 32 个扇区）      │
           └─────────────────────────────────────────┘
           │                                         │
           │    可用分区区域                           │
           │  (first_usable_lba ~ last_usable_lba)    │
           │                                         │
LBA N-33   ┌─────────────────────────────────────────┐
           │  GPT 分区表项数组（备份）                  │
           │  (Alternate Partition Entries)           │
           └─────────────────────────────────────────┘
LBA N-1    ┌─────────────────────────────────────────┐
           │  备份 GPT 头（Alternate GPT Header）      │
           │  - 随磁盘大小变化位置                      │
           └─────────────────────────────────────────┘
LBA N      （磁盘末尾）
```

#### 11.4.2 核心数据结构

**GPT 头**（[efi.h](file:///home/louis/code/linux/block/partitions/efi.h)）：

```c
typedef struct _gpt_header {
    __le64 signature;                // 签名: "EFI PART" (0x5452415020494645ULL)
    __le32 revision;                 // 修订版本 (0x00010000 = v1.0)
    __le32 header_size;              // 头大小 (通常 92 字节)
    __le32 header_crc32;             // 头 CRC32 校验（计算时此字段置 0）
    __le32 reserved1;                // 保留
    __le64 my_lba;                   // 本 GPT 头所在的 LBA
    __le64 alternate_lba;            // 备份 GPT 头所在的 LBA
    __le64 first_usable_lba;         // 第一个可用数据区的 LBA
    __le64 last_usable_lba;          // 最后一个可用数据区的 LBA
    efi_guid_t disk_guid;            // 磁盘唯一 GUID（16 字节）
    __le64 partition_entry_lba;      // 分区表项数组的起始 LBA
    __le32 num_partition_entries;    // 分区表项数量（默认 128）
    __le32 sizeof_partition_entry;   // 每个分区表项大小（默认 128 字节）
    __le32 partition_entry_array_crc32;  // 分区表项数组 CRC32
    // 剩余空间为零填充（BlockSize - 92 字节）
} __packed gpt_header;  // 共 92 字节
```

**GPT 分区表项**：

```c
typedef struct _gpt_entry {
    efi_guid_t partition_type_guid;    // 分区类型 GUID（16 字节）
    efi_guid_t unique_partition_guid;  // 分区唯一 GUID（16 字节）
    __le64 starting_lba;               // 起始 LBA（8 字节）
    __le64 ending_lba;                 // 结束 LBA（8 字节）
    gpt_entry_attributes attributes;   // 属性标志（8 字节）
    __le16 partition_name[36];         // 分区名称（36 个 UTF-16LE 字符, 72 字节）
} __packed gpt_entry;  // 共 128 字节
```

**GPT 分区属性标志**：

```c
typedef struct _gpt_entry_attributes {
    u64 required_to_function:1;  // 位 0: 分区必须存在才能正常工作
    u64 reserved:47;             // 位 1-47: 保留
    u64 type_guid_specific:16;   // 位 48-63: 类型 GUID 专用
} __packed gpt_entry_attributes;
```

**保护性 MBR 结构**（`legacy_mbr`）：

```c
typedef struct _legacy_mbr {
    u8 boot_code[440];            // 引导代码
    __le32 unique_mbr_signature;  // MBR 签名
    __le16 unknown;               // 未知
    gpt_mbr_record partition_record[4];  // 4 个 MBR 分区记录
    __le16 signature;             // 0xAA55 签名
} __packed legacy_mbr;  // 共 512 字节

typedef struct _gpt_mbr_record {
    u8 boot_indicator;    // 引导标志
    u8 start_head;        // 起始磁头（CHS, GPT 不使用）
    u8 start_sector;      // 起始扇区
    u8 start_track;       // 起始磁道
    u8 os_type;           // OS 类型（GPT: 0xEE, 混合 MBR: 其他类型）
    u8 end_head;          // 结束磁头
    u8 end_sector;        // 结束扇区
    u8 end_track;         // 结束磁道
    __le32 starting_lba;  // 起始 LBA（GPT 使用）
    __le32 size_in_lba;   // 大小（扇区数, GPT 使用）
} __packed gpt_mbr_record;
```

**常用分区类型 GUID**：

```c
#define PARTITION_SYSTEM_GUID           // {C12A7328-F81F-11D2-BA4B-00A0C93EC93B}  EFI 系统分区
#define LEGACY_MBR_PARTITION_GUID       // {024DEE41-33E7-11D3-9D69-0008C781F39F}  MBR 兼容分区
#define PARTITION_MSFT_RESERVED_GUID    // {E3C9E316-0B5C-4DB8-817D-F92DF00215AE}  Microsoft 保留分区
#define PARTITION_BASIC_DATA_GUID       // {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}  基本数据分区
#define PARTITION_LINUX_RAID_GUID       // {A19D880F-05FC-4D3B-A006-743F0F84911E}  Linux RAID
#define PARTITION_LINUX_SWAP_GUID       // {0657FD6D-A4AB-43C4-84E5-0933C84B4F4F}  Linux Swap
#define PARTITION_LINUX_LVM_GUID        // {E6D6D379-F507-44C2-A23C-238F2A3DF928}  Linux LVM
```

#### 11.4.3 GPT 解析流程

`efi_partition()` 的完整调用流程：

```text
efi_partition(state)
  │  # 入口: 调用 find_valid_gpt() 验证并获取有效的 GPT
  │
  ├─ 1. find_valid_gpt(state, &gpt, &ptes)
  │   │
  │   ├─ 1.1 读取 LBA 0（保护性 MBR）
  │   │   read_lba(state, 0, (u8 *)legacymbr, sizeof(*legacymbr))
  │   │
  │   ├─ 1.2 is_pmbr_valid(legacymbr, total_sectors)
  │   │   │  # 检查 PMBR 有效性
  │   │   │
  │   │   └─ 检查条件:
  │   │       ├─ 签名: le16_to_cpu(mbr->signature) == MSDOS_MBR_SIGNATURE (0xAA55)
  │   │       ├─ 分区类型: os_type == 0xEE (EFI_PMBR_OSTYPE_EFI_GPT)
  │   │       └─ starting_lba == 1 (GPT_PRIMARY_PARTITION_TABLE_LBA)
  │   │
  │   │   └─ 返回 GPT_MBR_PROTECTIVE 或 GPT_MBR_HYBRID
  │   │
  │   ├─ 1.3 is_gpt_valid(state, GPT_PRIMARY_PARTITION_TABLE_LBA, &pgpt, &pptes)
  │   │   │  # 验证主 GPT 头
  │   │   │
  │   │   └─ alloc_read_gpt_header(state, lba)
  │   │       │  # 分配并读取 GPT 头
  │   │       │
  │   │       └─ 验证步骤:
  │   │           ├─ signature == "EFI PART" (0x5452415020494645ULL)
  │   │           ├─ header_size <= 逻辑块大小
  │   │           ├─ header_size >= sizeof(gpt_header)
  │   │           ├─ header_crc32 校验通过（efi_crc32 重新计算）
  │   │           ├─ my_lba == lba 参数
  │   │           ├─ first_usable_lba <= lastlba
  │   │           ├─ last_usable_lba <= lastlba
  │   │           ├─ sizeof_partition_entry == sizeof(gpt_entry)
  │   │           ├─ 分区表大小不超出 KMALLOC_MAX_SIZE
  │   │           └─ partition_entry_array_crc32 校验通过
  │   │
  │   ├─ 1.4 is_gpt_valid(state, pgpt->alternate_lba, &agpt, &aptes)
  │   │   │  # 验证备份 GPT 头
  │   │   │  # 若主 GPT 有效, 才验证备份 GPT
  │   │   │  # 若 force_gpt 则强制从磁盘末尾读取备份 GPT
  │   │   │
  │   ├─ 1.5 compare_gpts(pgpt, agpt, lastlba)
  │   │   │  # 比对主/备 GPT 一致性
  │   │   │
  │   │   └─ 比对项目:
  │   │       ├─ pgpt->my_lba == agpt->alternate_lba
  │   │       ├─ pgpt->alternate_lba == agpt->my_lba
  │   │       ├─ first_usable_lba 一致
  │   │       ├─ last_usable_lba 一致
  │   │       ├─ disk_guid 一致
  │   │       ├─ num_partition_entries 一致
  │   │       ├─ sizeof_partition_entry 一致
  │   │       ├─ partition_entry_array_crc32 一致
  │   │       └─ alternate_lba == lastlba（备份 GPT 应在磁盘末尾）
  │   │
  │   └─ 1.6 选择有效的 GPT
  │       ├─ 若主 GPT 有效: 使用主 GPT（打印警告若备份 GPT 无效）
  │       └─ 若主无效但备份有效: 使用备份 GPT（打印警告）
  │
  └─ 2. 枚举分区表项
      └─ for i = 0..num_partition_entries, limit = state->limit-1:
          ├─ is_pte_valid(&ptes[i], lastlba):
          │   ├─ partition_type_guid != NULL_GUID
          │   ├─ starting_lba <= lastlba
          │   └─ ending_lba <= lastlba
          │
          ├─ put_partition(state, i+1, start * ssz, size * ssz)
          │
          ├─ 若 partition_type_guid == PARTITION_LINUX_RAID_GUID
          │   → 设置 ADDPART_FLAG_RAID 标志
          │
          ├─ efi_guid_to_str(&ptes[i].unique_partition_guid, info->uuid)
          │   → 将分区 GUID 转为 UUID 字符串
          │
          └─ utf16_le_to_7bit(ptes[i].partition_name, label_max, info->volname)
              → 将 UTF-16LE 分区名称转为 ASCII 字符串
```

**关键验证函数 `is_gpt_valid()` 的 CRC 校验逻辑**：

```c
static int is_gpt_valid(struct parsed_partitions *state, u64 lba,
                        gpt_header **gpt, gpt_entry **ptes)
{
    // 1. 读取 GPT 头
    *gpt = alloc_read_gpt_header(state, lba);

    // 2. 验证签名
    if (le64_to_cpu((*gpt)->signature) != GPT_HEADER_SIGNATURE)
        goto fail;

    // 3. 验证头大小范围
    if (le32_to_cpu((*gpt)->header_size) > queue_logical_block_size(...))
        goto fail;
    if (le32_to_cpu((*gpt)->header_size) < sizeof(gpt_header))
        goto fail;

    // 4. CRC 校验: 头 CRC 的计算将 header_crc32 字段置 0 后重新计算
    origcrc = le32_to_cpu((*gpt)->header_crc32);
    (*gpt)->header_crc32 = 0;
    crc = efi_crc32((const unsigned char *)(*gpt),
                    le32_to_cpu((*gpt)->header_size));
    if (crc != origcrc)
        goto fail;

    // 5. 验证 my_lba 一致性
    if (le64_to_cpu((*gpt)->my_lba) != lba)
        goto fail;

    // 6. 验证 first/last_usable_lba 范围
    // 7. 验证 sizeof_partition_entry
    // 8. 读取分区表项并验证 array CRC
    *ptes = alloc_read_gpt_entries(state, *gpt);
    crc = efi_crc32((const unsigned char *)(*ptes), pt_size);
    if (crc != le32_to_cpu((*gpt)->partition_entry_array_crc32))
        goto fail_ptes;

    return 1;  // 验证通过
}
```

#### 11.4.4 保护性 MBR（PMBR）

GPT 规范要求在 LBA 0 处放置一个保护性 MBR（Protective MBR），其目的是：

1. **兼容性**：让不支持 GPT 的旧操作系统/工具识别磁盘为"已分区"，避免误格式化
2. **保护**：防止旧工具误认为磁盘未分区而覆盖 GPT 数据

**PMBR 特征**：
- `signature` = 0xAA55（标准 MBR 签名）
- `partition_record[0].os_type` = 0xEE（EFI_PMBR_OSTYPE_EFI_GPT）
- `partition_record[0].starting_lba` = 1（指向 GPT 头）
- `partition_record[0].size_in_lba` = 整个磁盘大小或 0xFFFFFFFF（2TiB）

**混合 MBR（Hybrid MBR）**：
- 同时包含 GPT 分区和最多 3 个传统 MBR 分区表项（第 4 个保留为 0xEE）
- 用于支持双系统引导（如 macOS Boot Camp）
- 通过 `is_pmbr_valid()` 检测：若存在非 0xEE 且非 0x00 的分区类型，则判定为混合 MBR

```c
static int is_pmbr_valid(legacy_mbr *mbr, sector_t total_sectors)
{
    // 检查签名
    if (le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE)
        return 0;

    // 查找 0xEE 类型分区
    for (i = 0; i < 4; i++) {
        ret = pmbr_part_valid(&mbr->partition_record[i]);
        if (ret == GPT_MBR_PROTECTIVE) goto check_hybrid;
    }
    return 0;

check_hybrid:
    // 检查是否存在非 GPT 分区 → 混合 MBR
    for (i = 0; i < 4; i++)
        if (mbr->partition_record[i].os_type != 0xEE &&
            mbr->partition_record[i].os_type != 0x00)
            ret = GPT_MBR_HYBRID;
    return ret;
}
```

#### 11.4.5 主/备 GPT 冗余机制

GPT 在磁盘末尾保存一份备份（Alternate）GPT，提供数据冗余：

**冗余策略**：

```
正常情况:
  主 GPT (LBA 1)  ←──────────────→  备份 GPT (LBA N-1)
  主分区表 (LBA 2~33)  ←────────→  备份分区表 (LBA N-33~N-2)
  两者通过 alternate_lba 互相引用

主 GPT 损坏:
  find_valid_gpt() 发现主 GPT CRC 校验失败
  → 尝试加载备份 GPT
  → 如果备份有效, 使用备份 GPT（打印警告）
  → 用户可通过 kernel 参数 'gpt' 强制使用备份

备份 GPT 损坏:
  find_valid_gpt() 发现备份 GPT CRC 校验失败
  → 使用主 GPT（打印警告）
  → 用户空间工具（如 gdisk）可修复备份 GPT

主/备不一致:
  compare_gpts() 检测以下差异:
  - my_lba ↔ alternate_lba 交叉引用不匹配
  - first/last_usable_lba 不一致
  - disk_guid 不一致
  - 分区表数量/大小/CRC 不一致
  → 打印警告, 建议使用 GNU Parted 修复
```

**备份 GPT 定位策略**：

```c
// 正常情况: 从主 GPT 的 alternate_lba 字段获取
good_agpt = is_gpt_valid(state, le64_to_cpu(pgpt->alternate_lba), &agpt, &aptes);

// 强制模式: 从磁盘末尾读取
if (!good_agpt && force_gpt)
    good_agpt = is_gpt_valid(state, lastlba, &agpt, &aptes);

// 驱动特殊处理: 某些设备（如 Apple 磁盘）的备份 GPT 在特殊位置
if (!good_agpt && force_gpt && fops->alternative_gpt_sector)
    fops->alternative_gpt_sector(disk, &agpt_sector);
```

#### 11.4.6 GPT 功能特点

| 特性 | 说明 |
|------|------|
| 最大磁盘容量 | 无限制（64 位 LBA 寻址） |
| 最大分区数 | 默认 128 个（可扩展, 由 `num_partition_entries` 决定） |
| 分区表冗余 | 主/备双份 GPT, CRC 校验保护 |
| 分区标识 | 128 位 GUID, 全局唯一 |
| 分区名称 | 36 字符 UTF-16LE 名称 |
| 分区属性 | 8 字节属性标志位 |
| 兼容性 | 保护性 MBR 确保传统工具兼容 |
| 校验保护 | GPT 头 CRC32 + 分区表阵列 CRC32 |

### 11.5 MBR vs GPT 对比总结

| 对比维度 | DOS/MBR | EFI/GPT |
|----------|---------|---------|
| **规范起源** | IBM PC/AT, 1983 | UEFI 规范, 1999 |
| **实现文件** | `block/partitions/msdos.c` | `block/partitions/efi.c` |
| **代码行数** | 717 行 | 756 行 |
| **核心数据结构** | `struct msdos_partition` (16B) | `struct gpt_header` (92B) + `struct gpt_entry` (128B) |
| **分区表位置** | LBA 0, 偏移 0x1BE | LBA 1 (主), LBA N-1 (备份) |
| **分区表项大小** | 16 字节 | 128 字节 |
| **最大主分区数** | 4 个 | 默认 128 个 |
| **逻辑分区** | 通过扩展分区链式支持 | 不需要（直接支持 128+ 分区） |
| **最大磁盘容量** | 2 TiB (32 位 LBA) | 无限制 (64 位 LBA) |
| **分区标识** | 1 字节 `sys_ind` | 128 位 GUID |
| **唯一磁盘 ID** | 4 字节签名 | 16 字节 GUID |
| **分区名称** | 不支持 | 36 字符 UTF-16LE |
| **数据冗余** | 无 | 主/备 GPT 双份 + CRC32 |
| **校验保护** | 无 | 头 CRC32 + 分区表阵列 CRC32 |
| **兼容性** | 所有 x86 系统 | 需要保护性 MBR |
| **内核检测顺序** | 在 GPT 之后（`check_part[]` 中 GPT 先于 MBR） | 在 MBR 之前 |
| **检测入口** | `msdos_partition()` | `efi_partition()` |
| **检测逻辑** | 检查 0x55AA 签名 + boot_ind 有效性 | 验证 PMBR → 验证 GPT 头 CRC → 验证分区表 CRC |
| **扩展分区** | `parse_extended()` 链式遍历 | 无此概念 |
| **子分区** | BSD disklabel, Solaris VTOC, Minix 等 | 无 |

**内核检测优先级**：

```text
check_part[] 数组定义在 block/partitions/core.c:
──────────────────────────────────────────────────
1. ADFS (Acorn) 分区         # 优先检测（有 ADFS 引导块）
2. CMDLINE 分区               # 内核命令行指定
3. OF (设备树) 分区            # 设备树指定
4. EFI/GPT 分区               # ★ GPT 优先于 MBR ★
5. SGI 分区
6. LDM (Windows 动态磁盘)
7. MSDOS/MBR 分区             # ★ MBR 在 GPT 之后 ★
8. OSF/Unix, Sun, Amiga, Atari, Mac, ...
──────────────────────────────────────────────────
```

**为什么 GPT 检测在 MBR 之前**：
- GPT 磁盘的 LBA 0 包含保护性 MBR（0xEE 类型）
- 若先运行 MBR 检测，`msdos_partition()` 会看到 PMBR 并尝试解析为无效的 MBR
- 内核解决方案：在 `check_part[]` 数组中，`efi_partition` 排在 `msdos_partition` 之前
- `msdos_partition()` 内部也有保护逻辑：若发现 `sys_ind == 0xEE`，立即 `return 0` 让 GPT 处理

**检测流程决策树**：

```text
read_part_sector(LBA 0)
  │
  ├─ 0x55AA 签名存在?
  │   ├─ 否 → 非 MBR 磁盘, 尝试其他分区格式
  │   └─ 是 → 继续
  │
  ├─ sys_ind == 0xEE (GPT PMBR)?
  │   ├─ 是 → 跳过 MBR 解析, 交由 efi_partition() 处理
  │   │        └─ efi_partition():
  │   │            ├─ PMBR 验证通过?
  │   │            ├─ 主 GPT 头 CRC 验证?
  │   │            ├─ 备份 GPT 头 CRC 验证?
  │   │            └─ 分区表项 CRC 验证?
  │   │
  │   └─ 否 → 继续 MBR 解析
  │
  ├─ 扩展分区?
  │   └─ 是 → parse_extended() 链式解析逻辑分区
  │
  └─ 子分区类型?
      └─ BSD/Solaris/Unixware/Minix → 调用对应的子分区解析器
```

---

## 12. 数据完整性与加密

### 12.1 数据完整性（DIF/DIX）

T10 保护信息（T10 Protection Information, PI）是 SCSI 和 NVMe 设备支持的数据完整性方案，也称为 DIF（Data Integrity Field）或 DIX（Data Integrity Extension）。其核心思想是在每个数据扇区后附加一个 PI 元组，用于校验数据的完整性和正确性。

#### 12.1.1 体系架构

```text
                        文件系统 / 应用层
                              │
                              ▼
                    ┌──────────────────────┐
                    │  bio_integrity_prep() │  ← bio-integrity-auto.c
                    │  (自动生成/验证 PI)    │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  bio 层完整性管理      │
                    │  bio_integrity_alloc() │  ← bio-integrity.c
                    │  bio_integrity_free()  │
                    │  bio_integrity_add_page│
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  blk 层完整性管理      │
                    │  blk_integrity_generate│  ← t10-pi.c
                    │  blk_integrity_verify  │
                    │  blk_integrity_prepare │
                    │  blk_integrity_complete│
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  设备驱动层            │
                    │  (NVMe/SCSI 驱动)      │
                    │  硬件处理 PI 或传递    │
                    └──────────────────────┘
```

**三种完整性保护模式**：

| 模式 | 说明 | 数据流 |
|------|------|--------|
| **DIF Type 1** | 数据块 + 保护信息一起传输，ref_tag 为起始 LBA | 主机生成/验证 PI |
| **DIF Type 2** | 同 Type 1，但 ref_tag 间接引用 LBA1 | 主机生成/验证 PI |
| **DIF Type 3** | 数据块 + 保护信息一起传输，ref_tag 未定义 | 主机生成/验证 PI |
| **DIX** | 保护信息与数据分离传输，通过独立 DMA 通道 | 主机生成，设备验证 |

#### 12.1.2 核心数据结构

**设备级完整性描述**（[blkdev.h](file:///home/louis/code/linux/include/linux/blkdev.h)）：

```c
struct blk_integrity {
    unsigned char                   flags;         // 标志位:
                                                    //   BLK_INTEGRITY_NOVERIFY     = 1 << 0  (禁用读校验)
                                                    //   BLK_INTEGRITY_NOGENERATE   = 1 << 1  (禁用写生成)
                                                    //   BLK_INTEGRITY_DEVICE_CAPABLE = 1 << 2 (设备支持)
                                                    //   BLK_INTEGRITY_REF_TAG      = 1 << 3  (启用 ref_tag)
                                                    //   BLK_INTEGRITY_STACKED      = 1 << 4  (已堆叠)
    enum blk_integrity_checksum     csum_type;     // 校验和类型:
                                                    //   BLK_INTEGRITY_CSUM_NONE  = 0
                                                    //   BLK_INTEGRITY_CSUM_IP    = 1 (IP 校验和)
                                                    //   BLK_INTEGRITY_CSUM_CRC   = 2 (CRC16-T10DIF)
                                                    //   BLK_INTEGRITY_CSUM_CRC64 = 3 (CRC64-NVMe)
    unsigned char                   metadata_size; // 每个扇区的元数据总大小 (字节)
    unsigned char                   pi_offset;     // PI 元组在元数据中的偏移
    unsigned char                   interval_exp;  // 数据间隔指数 (2^interval_exp 字节/扇区)
    unsigned char                   tag_size;      // 应用标签大小
    unsigned char                   pi_tuple_size; // PI 元组大小
} __packed;  // 共 7 字节, 嵌入在 struct queue_limits 中
```

**Bio 级完整性负载**（[bio-integrity.h](file:///home/louis/code/linux/include/linux/bio-integrity.h)）：

```c
struct bio_integrity_payload {
    struct bvec_iter    bip_iter;       // 完整性数据迭代器
    unsigned short      bip_vcnt;       // 完整性 bio_vec 数量
    unsigned short      bip_max_vcnt;   // 分配的 bio_vec 槽位数
    unsigned short      bip_flags;      // 控制标志:
                                        //   BIP_BLOCK_INTEGRITY  = 1 << 0  (块层拥有)
                                        //   BIP_MAPPED_INTEGRITY = 1 << 1  (ref_tag 已重映射)
                                        //   BIP_COPY_USER        = 1 << 4  (内核 bounce buffer)
                                        //   BIP_CHECK_GUARD      = 1 << 5  (校验 guard)
                                        //   BIP_CHECK_REFTAG     = 1 << 6  (校验 ref_tag)
                                        //   BIP_CHECK_APPTAG     = 1 << 7  (校验 app_tag)
    u16                 app_tag;        // 应用标签值
    struct bio_vec      *bip_vec;       // 完整性数据页数组
};
```

**T10 PI 元组**（[t10-pi.h](file:///home/louis/code/linux/include/linux/t10-pi.h)）：

```c
struct t10_pi_tuple {
    __be16 guard_tag;   // 校验和 (2 字节): CRC16-T10DIF 或 IP 校验和
    __be16 app_tag;     // 应用标签 (2 字节): 上层应用可用
    __be32 ref_tag;     // 引用标签 (4 字节): 通常为起始 LBA 的低 32 位
} __packed;  // 共 8 字节

// 特殊转义值:
#define T10_PI_APP_ESCAPE  cpu_to_be16(0xffff)     // 应用标签逃逸
#define T10_PI_REF_ESCAPE  cpu_to_be32(0xffffffff)  // 引用标签逃逸
```

**T10 PI 类型定义**：

```c
enum t10_dif_type {
    T10_PI_TYPE0_PROTECTION = 0x0,  // 无保护
    T10_PI_TYPE1_PROTECTION = 0x1,  // ref_tag = LBA (标准)
    T10_PI_TYPE2_PROTECTION = 0x2,  // ref_tag 间接引用
    T10_PI_TYPE3_PROTECTION = 0x3,  // 无 ref_tag
};
```

**CRC64 PI 元组**（NVMe 扩展）：

```c
struct crc64_pi_tuple {
    __be64 guard_tag;   // CRC64 校验和 (8 字节)
    __be16 app_tag;     // 应用标签 (2 字节)
    __be16 ref_tag;     // 引用标签 (2 字节)
} __packed;  // 共 12 字节
```

#### 12.1.3 完整性校验和算法

```c
enum blk_integrity_checksum {
    BLK_INTEGRITY_CSUM_NONE  = 0,  // 无校验和
    BLK_INTEGRITY_CSUM_IP    = 1,  // IP 校验和 (16 位, 快速)
    BLK_INTEGRITY_CSUM_CRC   = 2,  // CRC16-T10DIF (16 位, 标准)
    BLK_INTEGRITY_CSUM_CRC64 = 3,  // CRC64-NVMe (64 位, NVMe 扩展)
};
```

- **IP 校验和**：`ip_compute_csum()`，基于 16 位补码加法，速度快但碰撞概率高于 CRC
- **CRC16-T10DIF**：`crc_t10dif()`，使用 SCSI 规范定义的 CRC16 多项式 `0x18BB7`
- **CRC64-NVMe**：NVMe 扩展的 64 位 CRC，提供更强的校验能力

#### 12.1.4 完整性 I/O 流程

**写路径**（生成 PI 元组）：

```text
bio_integrity_prep(bio)                     # bio-integrity-auto.c
  │
  ├─ 检查设备是否支持完整性 (blk_get_integrity)
  ├─ 检查 bio 是否已有完整性负载
  │
  ├─ 分配 bio_integrity_data (mempool)
  ├─ 调用 bio_integrity_init() 设置 bip
  ├─ 调用 bio_integrity_alloc_buf() 分配元数据缓冲区
  │   └─ kmalloc 或 mempool 备用
  ├─ 设置 seed (bi_sector)
  ├─ 设置 BIP_CHECK 标志 (根据 csum_type)
  │
  └─ 若为 WRITE 且需要校验:
      └─ blk_integrity_generate(bio)        # t10-pi.c
          │
          └─ 遍历 bio 的每个 data segment:
              ├─ t10_pi_generate(iter, bi)  # CRC16 / IP 校验和
              │   └─ 对每个 interval:
              │       ├─ 计算 guard_tag = t10_pi_csum(data, interval)
              │       ├─ app_tag = 0
              │       └─ ref_tag = lower_32_bits(seed)
              │
              └─ ext_pi_crc64_generate(iter, bi)  # CRC64 扩展
                  └─ 对每个 interval:
                      ├─ 计算 guard_tag = crc64(data, interval)
                      ├─ app_tag = 0
                      └─ ref_tag = lower_32_bits(seed)

blk_integrity_prepare(rq)                   # t10-pi.c
  └─ 若 BLK_INTEGRITY_REF_TAG:
      ├─ t10_pi_type1_prepare(rq)  # 为每个 bio 重映射 ref_tag
      └─ ext_pi_type1_prepare(rq)  # CRC64 版本
          └─ 遍历 request 中的每个 bio:
              └─ 用 rq->__sector 替换 bip 中的 seed
```

**读路径**（验证 PI 元组）：

```text
bio_integrity_prep(bio)                     # bio-integrity-auto.c
  │
  └─ 若为 READ:
      └─ 保存 bi_iter 到 saved_bio_iter (用于后续验证)

bio_integrity_endio(bio)                    # bio-integrity.c / blk.h
  │
  ├─ 检查 bip->bip_flags & BIP_BLOCK_INTEGRITY
  │
  └─ __bio_integrity_endio(bio)             # bio-integrity-auto.c
      │
      ├─ 若 READ 成功且需要校验:
      │   └─ queue_work(kintegrityd_wq, &bid->work)
      │       └─ bio_integrity_verify_fn()  # 工作队列处理
      │           └─ blk_integrity_verify_iter(bio, saved_iter)
      │               │
      │               └─ 遍历 bio 的每个 data segment:
      │                   ├─ t10_pi_verify(iter, bi)  # CRC16 / IP
      │                   │   └─ 对每个 interval:
      │                   │       ├─ 检查 app_tag 逃逸
      │                   │       ├─ 检查 ref_tag 匹配
      │                   │       └─ 检查 guard_tag 匹配
      │                   │
      │                   └─ ext_pi_crc64_verify(iter, bi)  # CRC64
      │                       └─ 类似地检查 guard/app/ref tag
      │
      └─ bio_integrity_finish(bid)
          └─ bio_endio(bio)  # 继续 I/O 完成

blk_integrity_complete(rq, nr_bytes)        # t10-pi.c
  └─ 若 BLK_INTEGRITY_REF_TAG:
      ├─ t10_pi_type1_complete(rq, nr_bytes)  # 恢复 ref_tag
      └─ ext_pi_type1_complete(rq, nr_bytes)  # CRC64 版本
```

#### 12.1.5 完整性校验步骤详解

**写入时生成**（`t10_pi_generate()`）：

```c
static void t10_pi_generate(struct blk_integrity_iter *iter,
                            struct blk_integrity *bi)
{
    for (i = 0; i < iter->data_size; i += iter->interval) {
        struct t10_pi_tuple *pi = iter->prot_buf + bi->pi_offset;

        // 1. 计算 guard_tag (数据块校验和)
        pi->guard_tag = t10_pi_csum(0, iter->data_buf, iter->interval,
                                    bi->csum_type);
        // 若 pi_offset > 0, 还需包含元数据前缀
        if (bi->pi_offset)
            pi->guard_tag = t10_pi_csum(pi->guard_tag, iter->prot_buf,
                                        bi->pi_offset, bi->csum_type);

        // 2. app_tag 清零
        pi->app_tag = 0;

        // 3. 设置 ref_tag (起始 LBA 低 32 位)
        if (bi->flags & BLK_INTEGRITY_REF_TAG)
            pi->ref_tag = cpu_to_be32(lower_32_bits(iter->seed));
        else
            pi->ref_tag = 0;

        iter->data_buf += iter->interval;
        iter->prot_buf += bi->metadata_size;
        iter->seed++;
    }
}
```

**读取时验证**（`t10_pi_verify()`）：

```c
static blk_status_t t10_pi_verify(struct blk_integrity_iter *iter,
                                  struct blk_integrity *bi)
{
    for (i = 0; i < iter->data_size; i += iter->interval) {
        struct t10_pi_tuple *pi = iter->prot_buf + bi->pi_offset;

        // Type 1: 检查 ref_tag
        if (bi->flags & BLK_INTEGRITY_REF_TAG) {
            if (pi->app_tag == T10_PI_APP_ESCAPE)
                goto next;  // 逃逸, 跳过校验
            if (be32_to_cpu(pi->ref_tag) != lower_32_bits(iter->seed))
                return BLK_STS_PROTECTION;  // ref_tag 错误!
        }

        // 重新计算 guard_tag 并与存储值比较
        csum = t10_pi_csum(0, iter->data_buf, iter->interval, bi->csum_type);
        if (bi->pi_offset)
            csum = t10_pi_csum(csum, iter->prot_buf, bi->pi_offset, bi->csum_type);

        if (pi->guard_tag != csum)
            return BLK_STS_PROTECTION;  // guard_tag 错误!
    }
    return BLK_STS_OK;
}
```

#### 12.1.6 完整性配置文件管理

**完整性配置验证**（[blk-settings.c](file:///home/louis/code/linux/block/blk-settings.c)）：

```c
static int blk_validate_integrity_limits(struct queue_limits *lim)
{
    struct blk_integrity *bi = &lim->integrity;

    // 1. 无元数据 → 禁用完整性, 设置 NOGENERATE + NOVERIFY
    if (!bi->metadata_size) {
        bi->flags |= BLK_INTEGRITY_NOGENERATE | BLK_INTEGRITY_NOVERIFY;
        return 0;
    }

    // 2. 检查 csum_type 和 REF_TAG 一致性
    if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE &&
        (bi->flags & BLK_INTEGRITY_REF_TAG))
        return -EINVAL;

    // 3. 检查 pi_offset + pi_tuple_size 不超过 metadata_size
    if (bi->pi_offset + bi->pi_tuple_size > bi->metadata_size)
        return -EINVAL;

    // 4. 校验每种 csum_type 的合法性
    switch (bi->csum_type) {
    case BLK_INTEGRITY_CSUM_NONE:
        if (bi->pi_tuple_size) return -EINVAL;
        break;
    case BLK_INTEGRITY_CSUM_CRC:
    case BLK_INTEGRITY_CSUM_IP:
        if (bi->pi_tuple_size != sizeof(struct t10_pi_tuple))
            return -EINVAL;
        break;
    case BLK_INTEGRITY_CSUM_CRC64:
        if (bi->pi_tuple_size != sizeof(struct crc64_pi_tuple))
            return -EINVAL;
        break;
    }
    return 0;
}
```

**完整性配置堆叠**（`queue_limits_stack_integrity()`）：

```c
bool queue_limits_stack_integrity(struct queue_limits *t,
                                  struct queue_limits *b)
{
    struct blk_integrity *ti = &t->integrity;
    struct blk_integrity *bi = &b->integrity;

    if (ti->flags & BLK_INTEGRITY_STACKED) {
        // 已堆叠: 检查与下层一致性
        if (ti->metadata_size != bi->metadata_size) goto incompatible;
        if (ti->interval_exp != bi->interval_exp) goto incompatible;
        if (ti->csum_type != bi->csum_type) goto incompatible;
        if (ti->pi_tuple_size != bi->pi_tuple_size) goto incompatible;
    } else {
        // 首次堆叠: 复制下层配置
        ti->flags = BLK_INTEGRITY_STACKED | ...;
        ti->csum_type = bi->csum_type;
        ti->metadata_size = bi->metadata_size;
        ti->interval_exp = bi->interval_exp;
        ti->tag_size = bi->tag_size;
        ti->pi_tuple_size = bi->pi_tuple_size;
        ti->pi_offset = bi->pi_offset;
    }
    return true;

incompatible:
    memset(ti, 0, sizeof(*ti));
    return false;
}
```

#### 12.1.7 完整性 sysfs 接口

通过 `blk_integrity_attr_group` 导出到 `/sys/block/<disk>/integrity/`：

| 属性 | 读/写 | 说明 |
|------|-------|------|
| `format` | RO | 完整性格式名称 (如 "T10-DIF-TYPE1-CRC") |
| `tag_size` | RO | 应用标签大小 (字节) |
| `protection_interval_bytes` | RO | 保护间隔大小 (字节) |
| `read_verify` | RW | 读取时验证 PI (0=启用, 1=禁用) |
| `write_generate` | RW | 写入时生成 PI (0=启用, 1=禁用) |
| `device_is_integrity_capable` | RO | 设备是否支持完整性 |

**完整性格式名称**（`blk_integrity_profile_name()`）：

```c
const char *blk_integrity_profile_name(struct blk_integrity *bi)
{
    switch (bi->csum_type) {
    case BLK_INTEGRITY_CSUM_IP:
        return bi->flags & BLK_INTEGRITY_REF_TAG ?
               "T10-DIF-TYPE1-IP" : "T10-DIF-TYPE3-IP";
    case BLK_INTEGRITY_CSUM_CRC:
        return bi->flags & BLK_INTEGRITY_REF_TAG ?
               "T10-DIF-TYPE1-CRC" : "T10-DIF-TYPE3-CRC";
    case BLK_INTEGRITY_CSUM_CRC64:
        return bi->flags & BLK_INTEGRITY_REF_TAG ?
               "EXT-DIF-TYPE1-CRC64" : "EXT-DIF-TYPE3-CRC64";
    default:
        return "nop";
    }
}
```

#### 12.1.8 完整性 I/O 合并控制

`blk-integrity.c` 提供两个合并控制函数，确保只有兼容的完整性生物/请求才能合并：

```c
// 检查两个请求的完整性是否可合并
bool blk_integrity_merge_rq(struct request_queue *q,
                            struct request *req, struct request *next)
{
    // 两者都有/都没有完整性负载
    // bip_flags 一致
    // 若检查 app_tag, 值必须一致
    // 合并后 integrity segments 不超限
    // 无 gap
}

// 检查 bio 是否能合并到已有请求
bool blk_integrity_merge_bio(struct request_queue *q,
                             struct request *req, struct bio *bio)
{
    // 类似检查, 用于 bio 合并到 request
}
```

### 12.2 块层内联加密（blk-crypto）

块层内联加密（Inline Encryption）允许存储设备硬件直接对数据进行加密/解密，避免数据在主机内存和磁盘之间传输时的明文暴露，同时提高性能。

#### 12.2.1 体系架构

```text
                        文件系统 / 应用层
                              │
                              ▼
                    ┌──────────────────────────┐
                    │  bio_crypt_set_ctx()      │
                    │  (设置加密上下文)          │
                    └──────────┬───────────────┘
                               │
                    ┌──────────▼───────────────┐
                    │  __blk_crypto_submit_bio()│  ← blk-crypto.c
                    │  (提交路径: 分配 keyslot)  │
                    └──────────┬───────────────┘
                               │
              ┌────────────────┼────────────────┐
              ▼                ▼                ▼
    ┌─────────────────┐ ┌───────────┐ ┌──────────────┐
    │ 硬件内联加密      │ │ 软件回退   │ │ 无加密        │
    │ (设备原生支持)    │ │ (fallback) │ │ (直接提交)    │
    │ blk-crypto-     │ │ blk-crypto│ │              │
    │ profile.c       │ │ -fallback │ │              │
    └─────────────────┘ └───────────┘ └──────────────┘
           │                  │
           ▼                  ▼
    ┌──────────────────────────────────────┐
    │  设备驱动层 (NVMe/UFS/emmc 等)        │
    │  硬件加密引擎或软件加密后提交          │
    └──────────────────────────────────────┘
```

#### 12.2.2 核心数据结构

**加密模式定义**（[blk-crypto.c](file:///home/louis/code/linux/block/blk-crypto.c)）：

```c
const struct blk_crypto_mode blk_crypto_modes[] = {
    [BLK_ENCRYPTION_MODE_AES_256_XTS] = {
        .name = "AES-256-XTS",
        .cipher_str = "xts(aes)",       // Linux Crypto API 名称
        .keysize = 64,                   // 256 位 XTS = 2 × 128 位密钥
        .security_strength = 32,         // 安全强度 (256 位)
        .ivsize = 16,                    // IV 大小 (128 位)
    },
    [BLK_ENCRYPTION_MODE_AES_128_CBC_ESSIV] = {
        .name = "AES-128-CBC-ESSIV",
        .cipher_str = "essiv(cbc(aes),sha256)",
        .keysize = 16,                   // 128 位
        .security_strength = 16,
        .ivsize = 16,
    },
    [BLK_ENCRYPTION_MODE_ADIANTUM] = {
        .name = "Adiantum",
        .cipher_str = "adiantum(xchacha12,aes)",
        .keysize = 32,                   // 256 位
        .security_strength = 32,
        .ivsize = 32,                    // 256 位 IV
    },
    [BLK_ENCRYPTION_MODE_SM4_XTS] = {
        .name = "SM4-XTS",
        .cipher_str = "xts(sm4)",
        .keysize = 32,                   // 256 位 SM4-XTS
        .security_strength = 16,
        .ivsize = 16,
    },
};
```

**加密密钥**（[blk-crypto.h](file:///home/louis/code/linux/include/linux/blk-crypto.h)）：

```c
struct blk_crypto_config {
    enum blk_crypto_mode_num crypto_mode;  // 加密算法
    unsigned int data_unit_size;           // 数据单元大小 (2^n 字节)
    unsigned int dun_bytes;                // DUN 字节数 (1~IV 大小)
    enum blk_crypto_key_type key_type;     // 密钥类型: RAW / HW_WRAPPED
};

struct blk_crypto_key {
    struct blk_crypto_config crypto_cfg;  // 加密配置
    unsigned int data_unit_size_bits;     // 数据单元大小的对数
    unsigned int size;                    // 密钥大小 (字节)
    u8 bytes[BLK_CRYPTO_MAX_RAW_KEY_SIZE]; // 密钥数据 (最大 64 字节)
};
```

**Bio 加密上下文**（[blk-crypto.h](file:///home/louis/code/linux/include/linux/blk-crypto.h)）：

```c
struct bio_crypt_ctx {
    const struct blk_crypto_key *bc_key;   // 加密密钥指针
    u64 bc_dun[BLK_CRYPTO_DUN_ARRAY_SIZE]; // 数据单元编号 (DUN)
};
```

- DUN（Data Unit Number）是每个数据单元的编号，类似于 IV
- 每个数据单元使用 `DUN` 作为 IV 进行加密，保证相同明文在不同位置产生不同密文
- `BLK_CRYPTO_DUN_ARRAY_SIZE = 2`，支持 128 位 DUN

**加密配置文件（Crypto Profile）**（[blk-crypto-profile.h](file:///home/louis/code/linux/include/linux/blk-crypto-profile.h)）：

```c
struct blk_crypto_ll_ops {
    // 编程密钥：将密钥写入硬件 keyslot
    int (*keyslot_program)(struct blk_crypto_profile *profile,
                          const struct blk_crypto_key *key,
                          unsigned int slot);
    // 擦除密钥：从硬件 keyslot 中删除密钥
    int (*keyslot_evict)(struct blk_crypto_profile *profile,
                        const struct blk_crypto_key *key,
                        unsigned int slot);
    // 生成硬件包装密钥
    int (*generate_key)(struct blk_crypto_profile *profile,
                       u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
    // 准备硬件包装密钥 (长期包装 → 短期包装)
    int (*prepare_key)(struct blk_crypto_profile *profile,
                      const u8 *lt_key, size_t lt_key_size,
                      u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
};

struct blk_crypto_profile {
    struct blk_crypto_ll_ops ll_ops;    // 驱动层操作函数
    unsigned int max_dun_bytes_supported; // 最大 DUN 字节数
    unsigned int key_types_supported;    // 支持密钥类型 (RAW/HW_WRAPPED)
    unsigned int modes_supported[BLK_ENCRYPTION_MODE_MAX]; // 位图: 支持的算法×数据单元大小
    struct device *dev;                  // 运行时电源管理设备

    // 以下字段由 blk_crypto_profile_init() 管理:
    struct blk_crypto_keyslot *slots;    // keyslot 数组
    unsigned int num_slots;              // keyslot 数量
    struct list_head idle_slots;         // 空闲 keyslot 链表
    struct hlist_head *slot_hashtable;   // keyslot 哈希表 (按密钥指针)
    struct rw_semaphore lock;            // 保护 keyslot 管理的读写锁
};
```

**Keyslot 结构**（[blk-crypto-profile.c](file:///home/louis/code/linux/block/blk-crypto-profile.c)）：

```c
struct blk_crypto_keyslot {
    atomic_t slot_refs;                    // 引用计数 (当前使用该 slot 的 I/O 请求数)
    struct list_head idle_slot_node;       // 空闲链表节点
    struct hlist_node hash_node;           // 哈希表节点
    const struct blk_crypto_key *key;      // 当前编程的密钥指针
    struct blk_crypto_profile *profile;    // 所属 profile
};
```

#### 12.2.3 加密 I/O 提交路径

```text
submit_bio(bio)
  │
  ├─ 若 bio->bi_crypt_context 存在:
  │   └─ __blk_crypto_submit_bio(bio)              # blk-crypto.c
  │       │
  │       ├─ 1. 检查 bio 是否有数据
  │       │
  │       ├─ 2. 检查设备是否原生支持该加密配置:
  │       │   ├─ 是 → 直接返回 true (继续提交到驱动)
  │       │   │
  │       │   └─ 否 → 检查 fallback 是否启用:
  │       │       ├─ 是 → blk_crypto_fallback_bio_prep(bio)
  │       │       │   ├─ WRITE: 加密后提交
  │       │       │   │   └─ blk_crypto_fallback_encrypt_bio(bio)
  │       │       │   │       ├─ 分配 bounce page
  │       │       │   │       ├─ 使用 crypto API 进行加密
  │       │       │   │       └─ 提交加密后的 bio
  │       │       │   │
  │       │       │   └─ READ: 标记为解密后完成
  │       │       │       └─ 替换 bi_end_io 为解密回调
  │       │       │           └─ blk_crypto_fallback_decrypt_endio
  │       │       │               └─ 工作队列: blk_crypto_fallback_decrypt_bio
  │       │       │
  │       │       └─ 否 → 返回错误 (BLK_STS_NOTSUPP)
  │       │
  │       └─ 3. 返回 true (bio 继续提交)
  │
  └─ blk_mq_submit_bio(rq)
      │
      ├─ __blk_crypto_rq_bio_prep(rq, bio, gfp)     # 复制加密上下文到 request
      │
      └─ blk_crypto_rq_get_keyslot(rq)               # 分配硬件 keyslot
          │
          └─ blk_crypto_get_keyslot(profile, key, &slot_ptr)  # blk-crypto-profile.c
              │
              ├─ 1. 哈希查找: 密钥是否已编程到某个 slot?
              │   ├─ 是 → 增加引用计数, 返回 slot
              │   └─ 否 → 继续
              │
              ├─ 2. 从空闲链表获取一个 slot
              │   ├─ 有 → 使用
              │   └─ 无 → 等待 (wait_event)
              │
              └─ 3. 调用 ll_ops.keyslot_program(profile, key, slot)
                  └─ 驱动将密钥编程到硬件
```

#### 12.2.4 软件加密回退（fallback）机制

当设备不支持硬件加密时，`blk-crypto-fallback.c` 提供软件加密回退：

```c
// fallback 使用 Linux Crypto API 进行软件加密
// 预分配资源:
//   - 100 个 keyslot (可通过参数调整)
//   - 每个 keyslot 包含每个加密模式一个 tfm
//   - 128 个预分配 fallback 上下文
//   - bounce page 内存池

bool blk_crypto_fallback_bio_prep(struct bio *bio)
{
    if (bio_data_dir(bio) == WRITE) {
        // 写入: 加密后提交
        // 1. 分配 fallback 上下文
        // 2. 分配 bounce page
        // 3. 使用 crypto API 加密数据
        // 4. 提交加密后的 bio (无加密上下文)
        // 5. 原始 bio 在加密完成后结束
        blk_crypto_fallback_encrypt_bio(bio);
        return false;  // bio 已被消费
    } else {
        // 读取: 标记为解密后完成
        // 1. 替换 bi_end_io 为 blk_crypto_fallback_decrypt_endio
        // 2. 提交原始 bio (无加密上下文)
        // 3. 完成时: 工作队列中解密数据
        // 4. 恢复原始 bi_end_io 并调用
        return true;   // bio 继续提交
    }
}
```

**fallback 加密流程**：

```
WRITE:  bio → [加密] → 加密后的 bio → 设备
                        ↓
                   bounce pages
                   (明文 → 密文)

READ:   bio → 设备 → [解密] → 解密后的 bio
                      ↓
                工作队列处理
                (密文 → 明文)
```

#### 12.2.5 密钥管理

**密钥初始化**（`blk_crypto_init_key()`）：

```c
int blk_crypto_init_key(struct blk_crypto_key *blk_key,
                        const u8 *key_bytes, size_t key_size,
                        enum blk_crypto_key_type key_type,
                        enum blk_crypto_mode_num crypto_mode,
                        unsigned int dun_bytes,
                        unsigned int data_unit_size)
{
    // 1. 验证加密模式有效性
    // 2. 验证密钥大小 (RAW 模式必须匹配 keysize)
    // 3. 验证 HW_WRAPPED 密钥大小范围
    // 4. 验证 dun_bytes 范围 (1 ~ ivsize)
    // 5. 验证 data_unit_size 为 2 的幂
    // 6. 填充 blk_crypto_key
}
```

**Bio 加密上下文设置**（`bio_crypt_set_ctx()`）：

```c
void bio_crypt_set_ctx(struct bio *bio, const struct blk_crypto_key *key,
                       const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
                       gfp_t gfp_mask)
{
    struct bio_crypt_ctx *bc = mempool_alloc(bio_crypt_ctx_pool, gfp_mask);
    bc->bc_key = key;
    memcpy(bc->bc_dun, dun, sizeof(bc->bc_dun));
    bio->bi_crypt_context = bc;
}
```

**密钥擦除**（`__blk_crypto_evict_key()`）：

```c
int __blk_crypto_evict_key(struct blk_crypto_profile *profile,
                           const struct blk_crypto_key *key)
{
    // 1. 哈希查找 key 所在的 slot
    // 2. 等待 slot 引用计数降为 0
    // 3. 调用 ll_ops.keyslot_evict() 从硬件擦除密钥
    // 4. 从哈希表移除
    // 5. 将 slot 移回空闲链表
}
```

#### 12.2.6 DUN 处理

DUN（Data Unit Number）确保加密后的数据在不同位置具有唯一性：

```c
// DUN 递增 (多字节大整数加法)
void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
                             unsigned int inc)
{
    for (int i = 0; inc && i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) {
        dun[i] += inc;
        if (dun[i] < inc)  // 溢出 → 进位
            inc = 1;
        else
            inc = 0;
    }
}

// DUN 连续检查 (用于 bio 合并判断)
bool bio_crypt_dun_is_contiguous(const struct bio_crypt_ctx *bc,
                                 unsigned int bytes,
                                 const u64 next_dun[BLK_CRYPTO_DUN_ARRAY_SIZE])
{
    // 计算 bc->bc_dun + bytes 是否等于 next_dun
}

// 加密上下文合并检查
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1,
                             unsigned int bc1_bytes,
                             struct bio_crypt_ctx *bc2)
{
    // 1. 检查密钥是否相同 (同一把密钥)
    // 2. 检查 DUN 是否连续
}
```

#### 12.2.7 加密配置文件注册

```c
// 设备驱动调用 blk_crypto_register() 注册加密能力
// 定义在 include/linux/blkdev.h
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
bool blk_crypto_register(struct blk_crypto_profile *profile,
                         struct request_queue *q);
#endif

// 驱动实现步骤:
// 1. blk_crypto_profile_init(&profile, num_slots)
// 2. 填充 profile.ll_ops (keyslot_program, keyslot_evict, ...)
// 3. 填充 profile.modes_supported (支持哪些算法和数据单元大小)
// 4. 调用 blk_crypto_register(&profile, q)
```

#### 12.2.8 加密与完整性互斥

```c
// bio-integrity.c 中禁止完整性 + 加密同时使用
if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
    return ERR_PTR(-EOPNOTSUPP);

// 这是设计限制: 硬件加密会修改数据, 使 PI 校验和失效
```

### 12.3 SED/Opal 自加密驱动器

TCG Opal 是自加密驱动器（Self-Encrypting Drive, SED）的安全标准。Linux 内核通过 `sed-opal.c` 提供 Opal 驱动管理功能。

#### 12.3.1 架构概述

```text
                    用户空间 (sedutil / libata)
                          │
                          ▼
                    ┌──────────────────────┐
                    │  sed-opal ioctl       │
                    │  (SED_OPAL_*)         │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  sed-opal.c          │
                    │  (3,351 行)          │
                    │  TCG Opal 协议实现    │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  opal_proto.h        │
                    │  (485 行)            │
                    │  Opal 协议命令行定义  │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  块设备驱动           │
                    │  (ATA/NVMe 命令)      │
                    └──────────────────────┘
```

#### 12.3.2 核心数据结构

```c
struct opal_dev {
    u32             flags;            // 标志位
    void            *data;            // 驱动私有数据
    sec_send_recv   *send_recv;       // 发送/接收安全命令的函数指针
    struct mutex    dev_lock;         // 设备互斥锁
    u16             comid;            // 通信 ID
    u32             hsn;              // 主机会话号
    u32             tsn;              // 目标会话号
    u64             align;            // 对齐粒度
    u64             lowest_lba;       // 最低 LBA
    u32             logical_block_size; // 逻辑块大小
    u8              align_required;   // 是否需要对齐
    size_t          pos;              // 命令缓冲区当前位置
    u8              *cmd;             // 命令缓冲区
    u8              *resp;            // 响应缓冲区
};

// Opal 命令步骤 (分步执行)
struct opal_step {
    int (*fn)(struct opal_dev *dev, void *data);
    void *data;
};
```

#### 12.3.3 支持的 opal 操作

通过 `sed-opal.c` 的 ioctl 接口，用户空间可执行以下操作：

| 操作 | 功能 |
|------|------|
| `SED_OPAL_LOCK_UNLOCK` | 锁定/解锁 Opal 范围 |
| `SED_OPAL_ADD_LOCKING_RANGE` | 添加锁定范围 |
| `SED_OPAL_ERASE_LOCKING_RANGE` | 擦除锁定范围数据 |
| `SED_OPAL_ACTIVATE_LSP` | 激活锁 SP |
| `SED_OPAL_SETUP_LSP` | 设置锁 SP 密码 |
| `SED_OPAL_MBR_DONE` | 完成 MBR 阴影 |
| `SED_OPAL_WRITE_MBR` | 写入 MBR 阴影数据 |
| `SED_OPAL_TPR` | 管理权限 |
| `SED_OPAL_PW_LIFECYCLE` | 管理密码生命周期 |
| `SED_OPAL_SECURE_ERASE` | 安全擦除 |

#### 12.3.4 Opal 协议层（[opal_proto.h](file:///home/louis/code/linux/block/opal_proto.h)）

Opal 协议基于 TCG 存储安全标准，使用 Tiny Atom 编码：

```c
// Opal 原子编码格式
enum opal_atom_width {
    OPAL_WIDTH_TINY,    // 1 字节 (6 位数据 + 2 位控制)
    OPAL_WIDTH_SHORT,   // 2 字节
    OPAL_WIDTH_MEDIUM,  // 3 字节
    OPAL_WIDTH_LONG,    // 6 字节
    OPAL_WIDTH_TOKEN,   // 可变长度 token
};

// Opal 响应 token 解析
struct opal_resp_tok {
    const u8 *pos;                    // token 在缓冲区中的位置
    size_t len;                       // token 长度
    enum opal_response_token type;    // token 类型
    enum opal_atom_width width;       // 编码宽度
    union {
        u64 u;                        // 无符号值
        s64 s;                        // 有符号值
    } stored;
};
```

### 12.4 数据完整性与加密对比总结

| 特性 | 数据完整性 (DIF/DIX) | 块层内联加密 (blk-crypto) | SED/Opal |
|------|----------------------|--------------------------|----------|
| **核心文件** | `blk-integrity.c`, `bio-integrity.c`, `t10-pi.c` | `blk-crypto.c`, `blk-crypto-profile.c`, `blk-crypto-fallback.c` | `sed-opal.c`, `opal_proto.h` |
| **总代码行数** | ~1,645 行 | ~1,923 行 | ~3,836 行 |
| **目的** | 检测数据损坏/篡改 | 防止数据泄露 | 防止数据泄露 (磁盘级) |
| **实现位置** | 块层 (bio/request 层) | 块层 (bio/request 层) | 用户空间 ioctl 触发 |
| **硬件要求** | 可选 (可软件生成/验证) | 可选 (有 fallback) | 必需 (硬件加密引擎) |
| **标准** | T10 SCSI / NVMe PI | 无统一标准 | TCG Opal 2.0 |
| **数据单元** | 扇区 (512B/4KB) | 可配置 (512B~64KB) | 整个磁盘或 LBA 范围 |
| **密钥管理** | 无 | 内核管理 (keyslot) | 驱动器内部管理 |
| **性能影响** | 低 (硬件校验) | 低 (硬件加密) / 中 (fallback) | 无 (硬件加密) |
| **覆盖范围** | 每个扇区的校验和 | 每个数据单元的加密 | 整个磁盘加密 |
| **与加密互斥** | — | 不兼容完整性 | 独立于块层 |
| **配置方式** | sysfs 属性 | ioctl + crypto profile | ioctl |

**I/O 路径中的调用点**：

```text
提交路径 (submit)                     完成路径 (complete)
──────────────────────────────────    ──────────────────────────────────
↓ bio_integrity_prep()                ↓ bio_integrity_endio()
  → 分配/生成 PI (写)                    → 验证 PI (读, 工作队列)
  → 保存 iter (读)                      → 释放 PI 缓冲区
                                       ↓ blk_integrity_complete()
↓ __blk_crypto_submit_bio()              → 恢复 ref_tag
  → 分配 keyslot
  → 硬件加密或 fallback
                                       ↓ blk_crypto_fallback 完成
↓ blk_mq_submit_bio()                    → 解密数据 (读)
  → __blk_crypto_rq_bio_prep()           → 恢复原始 bi_end_io
  → blk_crypto_rq_get_keyslot()
```

---

## 13. Zoned 块设备

### 13.1 blk-zoned.c（2,363 行）

文件：`block/blk-zoned.c`

实现 Zoned Block Device（ZBD）支持，包括：

- **区域状态管理**：定义了 9 种区域状态（`zone_cond_name[]`）：
  - `NOT_WP`（非写指针）、`EMPTY`、`IMP_OPEN`（隐式打开）、`EXP_OPEN`（显式打开）、`CLOSED`、`READONLY`、`FULL`、`OFFLINE`、`ACTIVE`
- **Zone Write Plug**：每个 zone 一个写插件，通过哈希表管理，支持 BIO 插件化处理。
- Zone Append 操作支持。
- Zone Reset 和 Zone Finish 管理。

---

## 14. Sysfs 与调试接口

### 14.1 blk-sysfs.c（1,030 行）

文件：`block/blk-sysfs.c`

通过 sysfs 导出块层队列属性，用户空间可通过 `cat /sys/block/<dev>/queue/<attr>` 查看和修改队列参数。

#### 14.1.1 属性描述结构

每个属性由 `struct queue_sysfs_entry` 描述，支持 show/store 以及 limit 版本的 show_limit/store_limit：

```c
struct queue_sysfs_entry {
    struct attribute attr;
    ssize_t (*show)(struct gendisk *disk, char *page);
    ssize_t (*show_limit)(struct gendisk *disk, char *page);
    ssize_t (*store)(struct gendisk *disk, const char *page, size_t count);
    int (*store_limit)(struct gendisk *disk, const char *page,
            size_t count, struct queue_limits *lim);
};
```

区分两种 show/store 函数：
- **普通版**（show/store）：直接操作 `request_queue` 字段，如 `nr_requests`、`read_ahead_kb`
- **Limit 版**（show_limit/store_limit）：通过 `queue_limits` 机制，支持原子提交和回滚。写操作调用 `queue_limits_start_update()` → `store_limit` → `queue_limits_commit_update_frozen()`，确保一致性。

#### 14.1.2 属性分组

属性分为两组，分别以 `struct attribute_group` 管理：

**`queue_attrs`** — 通用属性（bio-based 和 request-based 队列均可见）：

| 属性 | 类型 | 说明 |
|------|------|------|
| `max_hw_sectors_kb` | RO | 硬件最大扇区数（KB） |
| `max_sectors_kb` | RW | 用户可调最大扇区数（KB） |
| `max_segments` | RO | 最大段数 |
| `max_discard_segments` | RO | 最大丢弃段数 |
| `max_integrity_segments` | RO | 最大完整性段数 |
| `max_segment_size` | RO | 最大段大小 |
| `max_write_streams` | RO | 最大写流数 |
| `write_stream_granularity` | RO | 写流粒度 |
| `logical_block_size` | RO | 逻辑块大小 |
| `physical_block_size` | RO | 物理块大小 |
| `chunk_sectors` | RO | 块对齐扇区数 |
| `minimum_io_size` | RO | 最小 I/O 大小 |
| `optimal_io_size` | RO | 最优 I/O 大小 |
| `discard_granularity` | RO | 丢弃粒度 |
| `discard_max_bytes` | RW | 用户可调最大丢弃字节数 |
| `discard_max_hw_bytes` | RO | 硬件最大丢弃字节数 |
| `write_zeroes_max_bytes` | RO | 最大写零字节数 |
| `zone_append_max_bytes` | RO | Zone Append 最大字节数 |
| `zone_write_granularity` | RO | Zone 写粒度 |
| `zoned` | RO | Zoned 设备类型 |
| `max_open_zones` | RO | 最大打开 zone 数 |
| `max_active_zones` | RO | 最大活跃 zone 数 |
| `rotational` | RW | 是否为旋转设备 |
| `iostats` | RW | I/O 统计开关 |
| `add_random` | RW | 是否贡献熵池 |
| `stable_writes` | RW | 稳定写开关 |
| `write_cache` | RW | 写缓存策略（write back/write through） |
| `fua` | RO | FUA 支持 |
| `dax` | RO | DAX 支持 |
| `virt_boundary_mask` | RO | 虚拟边界掩码 |
| `dma_alignment` | RO | DMA 对齐要求 |
| `read_ahead_kb` | RW | 预读大小（KB） |
| `hw_sector_size` | RO | 硬件扇区大小（logical_block_size 别名） |
| `atomic_write_max_bytes` | RO | 原子写最大字节数 |
| `atomic_write_unit_max_bytes` | RO | 原子写单元最大字节数 |
| `atomic_write_unit_min_bytes` | RO | 原子写单元最小字节数 |

**`blk_mq_queue_attrs`** — 仅 request-based（blk-mq）队列可见：

| 属性 | 类型 | 说明 |
|------|------|------|
| `scheduler` | RW | 当前 I/O 调度器（切换/查看） |
| `nr_requests` | RW | 队列最大请求数 |
| `async_depth` | RW | 异步请求深度 |
| `wbt_lat_usec` | RW | WBT 延迟目标（微秒） |
| `rq_affinity` | RW | 请求 CPU 亲和性 |
| `io_timeout` | RW | I/O 超时时间（毫秒） |

#### 14.1.3 属性可见性控制

`queue_attr_visible()` 和 `blk_mq_queue_attr_visible()` 控制属性可见性：

- `max_open_zones` / `max_active_zones`：仅对 zoned 设备可见
- blk-mq 属性组：仅对 request-based 队列可见
- `io_timeout`：仅当驱动实现了 `timeout` 回调时可见

#### 14.1.4 队列注册与注销

```text
# 注册流程
blk_register_queue(disk)
  ├─ kobject_add(&disk->queue_kobj, ...)       # 创建 queue 目录
  ├─ blk_mq_sysfs_register(disk)               # 注册 blk-mq 特定属性
  ├─ debugfs_create_dir(disk->disk_name, ...)   # 创建 debugfs 目录
  ├─ blk_mq_debugfs_register(q)                # 注册 debugfs 文件
  ├─ disk_register_independent_access_ranges()  # 注册独立访问范围
  ├─ blk_crypto_sysfs_register(disk)           # 注册加密属性
  ├─ elevator_set_default(q)                    # 设置默认调度器
  └─ kobject_uevent(&disk->queue_kobj, KOBJ_ADD) # 发送 uevent

# 注销流程
blk_unregister_queue(disk)
  ├─ blk_trace_shutdown(q)                     # 关闭 blktrace
  ├─ debugfs_remove_recursive(q->debugfs_dir)   # 移除 debugfs 目录
  ├─ blk_mq_sysfs_unregister(disk)             # 注销 blk-mq 属性
  └─ kobject_del(&disk->queue_kobj)            # 删除 queue 目录
```

### 14.2 blk-mq-debugfs.c / blk-mq-debugfs.h

文件：`block/blk-mq-debugfs.c` / `block/blk-mq-debugfs.h`

提供 debugfs 调试接口，挂载点：`/sys/kernel/debug/block/<disk>/`。

#### 14.2.1 队列级 debugfs 文件

| 文件 | 权限 | 功能 |
|------|------|------|
| `poll_stat` | 0400 | 轮询统计（当前为空） |
| `requeue_list` | 0400 | 显示被重新入队的请求列表 |
| `pm_only` | 0600 | 显示 PM-only 计数器值 |
| `state` | 0600 | 显示/修改队列状态标志 |
| `zone_wplugs` | 0400 | 显示 zone write plug 状态 |

**`state` 文件**：可写入 `run`、`start`、`kick` 操作队列：
```c
static ssize_t queue_state_write(void *data, const char __user *buf,
                 size_t count, loff_t *ppos)
{
    // "run"   → blk_mq_run_hw_queues(q, true)
    // "start" → blk_mq_start_stopped_hw_queues(q, true)
    // "kick"  → blk_mq_kick_requeue_list(q)
}
```

**队列状态标志**：通过 `blk_flags_show()` 以符号名显示所有 `QUEUE_FLAG_*` 位：

| 标志 | 含义 |
|------|------|
| `QUEUE_FLAG_DYING` | 队列正在销毁 |
| `QUEUE_FLAG_NOMERGES` | 禁止合并 |
| `QUEUE_FLAG_SAME_COMP` | 同 CPU 完成 |
| `QUEUE_FLAG_FAIL_IO` | 模拟 I/O 失败 |
| `QUEUE_FLAG_STATS` | 统计已启用 |
| `QUEUE_FLAG_REGISTERED` | 已注册 sysfs |
| `QUEUE_FLAG_QUIESCED` | 已静默 |
| `QUEUE_FLAG_QOS_ENABLED` | QoS 已启用 |
| `QUEUE_FLAG_BIO_ISSUE_TIME` | 记录 BIO 发起时间 |

#### 14.2.2 硬件队列级 debugfs 文件

| 文件 | 功能 |
|------|------|
| `state` | 硬件队列状态（STOPPED/TAG_ACTIVE/SCHED_RESTART/INACTIVE） |
| `flags` | 硬件队列标志（TAG_QUEUE_SHARED/STACKING/BLOCKING 等） |
| `dispatch` | 派发队列中的请求列表 |
| `busy` | 显示所有正在处理的请求 |
| `tags` | 标签分配器信息（nr_tags, active_queues, bitmap） |
| `tags_bitmap` | 标签位图 |
| `sched_tags` | 调度器标签分配器信息 |
| `sched_tags_bitmap` | 调度器标签位图 |
| `active` | 当前活跃请求数 |
| `dispatch_busy` | 派发忙计数 |
| `type` | 硬件队列类型（default/read/poll） |
| `ctx_map` | CPU 上下文映射位图 |

**请求显示格式**：`__blk_mq_debugfs_rq_show()` 输出每个请求的详细信息：
```
{.op=READ, .cmd_flags=REQ_SYNC|REQ_META, .rq_flags=RQF_STARTED|RQF_STATS,
 .state=in_flight, .tag=42, .internal_tag=-1}
```

#### 14.2.3 软件队列（ctx）级 debugfs 文件

| 文件 | 功能 |
|------|------|
| `read_rq_list` | 读请求列表 |
| `write_rq_list` | 写请求列表 |
| `poll_rq_list` | 轮询请求列表 |

#### 14.2.4 注册与注销

```c
void blk_mq_debugfs_register(struct request_queue *q);       // 注册队列级文件
void blk_mq_debugfs_register_hctxs(struct request_queue *q); // 注册所有 hctx
void blk_mq_debugfs_register_sched(struct request_queue *q); // 注册调度器文件
void blk_mq_debugfs_register_rq_qos(struct request_queue *q); // 注册 QoS 文件
```

### 14.3 blktrace — 块层跟踪

块层通过 `include/trace/events/block.h` 定义了一套完整的 tracepoint 系统，可在运行时通过 ftrace / perf 捕获。

#### 14.3.1 请求级 Tracepoints

| Tracepoint | 触发时机 | 关键参数 |
|-----------|---------|---------|
| `block_rq_insert` | 请求插入队列 | dev, sector, nr_sector, rwbs, ioprio, comm |
| `block_rq_issue` | 请求下发到驱动 | dev, sector, nr_sector, rwbs, ioprio, comm |
| `block_rq_complete` | 请求完成 | dev, sector, nr_sector, error, ioprio, rwbs |
| `block_rq_error` | 请求出错 | dev, sector, nr_sector, error, ioprio, rwbs |
| `block_rq_requeue` | 请求重新入队 | dev, sector, nr_sector, rwbs, ioprio |
| `block_rq_merge` | 请求合并 | dev, sector, nr_sector, rwbs, bytes |

#### 14.3.2 Bio 级 Tracepoints

| Tracepoint | 触发时机 | 关键参数 |
|-----------|---------|---------|
| `block_bio_complete` | bio 完成 | dev, sector, nr_sector, error, rwbs |
| `block_bio_queue` | bio 入队 | dev, sector, nr_sector, rwbs, comm |
| `block_bio_backmerge` | bio 向后合并 | dev, sector, nr_sector, rwbs |
| `block_bio_frontmerge` | bio 向前合并 | dev, sector, nr_sector, rwbs |
| `block_bio_remap` | bio 重映射 | dev, sector, nr_sector, old_dev, old_sector |
| `block_split` | bio 拆分 | dev, sector, new_sector, rwbs |
| `block_getrq` | 分配请求 | dev, sector, nr_sector, rwbs |

#### 14.3.3 Buffer Head 级 Tracepoints

| Tracepoint | 触发时机 |
|-----------|---------|
| `block_touch_buffer` | 访问 buffer_head |
| `block_dirty_buffer` | 标记 buffer_head 脏 |

#### 14.3.4 其他 Tracepoints

| Tracepoint | 触发时机 |
|-----------|---------|
| `block_plug` | 队列插上 |
| `block_unplug` | 队列拔插（含请求数） |
| `block_rq_remap` | 请求重映射 |
| `blk_zone_append_update_request_bio` | Zone Append 完成更新 sector |
| `blkdev_zone_mgmt` | Zone 管理操作 |
| `disk_zone_wplug_add_bio` | 向 zone write plug 添加 bio |
| `blk_zone_wplug_bio` | Zone write plug 处理 bio |

#### 14.3.5 输出格式说明

`rwbs` 字段是一个 5 字符的 I/O 操作描述串，由 `blk_fill_rwbs()` 生成：

| 字符位置 | 含义 |
|---------|------|
| 1 | R=读, W=写, D=丢弃, T=flush, A=zone append |
| 2 | R=读取, W=写入, B=屏障（废弃） |
| 3 | S=同步, F=force_unit_access |
| 4 | A=预读, M=元数据 |
| 5 | M=meta, S=同步（复用） |

**典型输出示例**：
```
  <...>-12345 [000] ...1 123.456: block_rq_issue: 8,0 W 4096 () 12345 + 8 [dd]
  <...>-12345 [000] d... 123.789: block_rq_complete: 8,0 W () 12345 + 8 [0]
```

### 14.4 ioctl.c（975 行）

文件：`block/ioctl.c`

处理块设备 ioctl 系统调用，主要功能分组：

**分区管理**：
- `BLKPG_ADD_PARTITION` — 添加分区（`blkpg_do_ioctl()` → `bdev_add_partition()`）
- `BLKPG_DEL_PARTITION` — 删除分区（`bdev_del_partition()`）
- `BLKPG_RESIZE_PARTITION` — 调整分区大小

**设备信息查询**：
- `BLKGETSIZE` / `BLKGETSIZE64` — 获取设备大小
- `BLKSSZGET` — 获取逻辑块大小
- `BLKPBSZGET` — 获取物理块大小
- `BLKALIGNOFF` — 获取对齐偏移
- `BLKROGET` / `BLKROSET` — 获取/设置只读状态
- `BLKRRPART` — 重新读取分区表

**I/O 参数控制**：
- `BLKFLSBUF` — 刷新缓冲区
- `BLKROTATIONAL` — 获取/设置旋转标志
- `BLKRASET` / `BLKRAGET` — 设置/获取预读大小
- `BLKFRASET` / `BLKFRAGET` — 设置/获取文件预读大小
- `BLKSECTGET` — 获取最大扇区数
- `BLKIOMIN` / `BLKIOOPT` — 获取最小/最优 I/O 大小
- `BLKDISCARD` / `BLKSECDISCARD` — 丢弃/安全丢弃扇区
- `BLKZEROOUT` — 写零
- `BLKWSAME` — 写相同数据

**多队列管理**：
- `BLKTRACESETUP` / `BLKTRACESTART` / `BLKTRACESTOP` / `BLKTRACETEARDOWN` — blktrace 控制
- `BLKSTONRA` — 设置 NVMe 流数量

### 14.5 fops.c（978 行）

文件：`block/fops.c`

实现块设备文件操作集 `def_blk_fops`：

```c
const struct file_operations def_blk_fops = {
    .open           = blkdev_open,       // 打开块设备
    .release        = blkdev_release,    // 关闭块设备
    .read_iter      = blkdev_read_iter,  // 读（使用 iov_iter）
    .write_iter     = blkdev_write_iter, // 写（使用 iov_iter）
    .mmap           = blkdev_mmap,       // 内存映射
    .fsync          = blkdev_fsync,      // 同步刷盘
    .unlocked_ioctl = blkdev_ioctl,      // ioctl 处理
    .compat_ioctl   = compat_blkdev_ioctl, // 32 位兼容 ioctl
    .splice_read    = blkdev_splice_read,  // splice 读
    .splice_write   = blkdev_splice_write, // splice 写
    .iopoll         = blkdev_iopoll,       // 轮询 I/O 完成
};
```

**读路径**：`blkdev_read_iter()` → `blkdev_read_folio()` / `__blkdev_direct_IO_simple()` / `__blkdev_direct_IO()`：
- 小块 I/O 使用 `blkdev_read_folio()`（通过页缓存）
- 大块 I/O 使用直接 I/O：`__blkdev_direct_IO_simple()`（单 bio）或 `__blkdev_direct_IO()`（多 bio、异步）

**写路径**：`blkdev_write_iter()` → `__blkdev_direct_IO_simple()` / `__blkdev_direct_IO()`：
- 始终使用直接 I/O，块设备不支持写回缓存
- 支持 `RWF_DSYNC` / `RWF_SYNC` 标志（通过 `REQ_FUA`）

**原子写**：当 `iocb->ki_flags & IOCB_ATOMIC` 时，bio 添加 `REQ_ATOMIC` 标志。

### 14.6 bsg.c / bsg-lib.c

文件：`block/bsg.c`（277 行）/ `block/bsg-lib.c`（412 行）

实现块层 SCSI 通用（BSG）接口，提供从用户空间直接发送 SCSI 命令到块设备的通道：

- **bsg.c**：字符设备接口层，管理 `bsg_device` 结构，处理 `SG_IO` 和 `SCSI_IOCTL` 命令
- **bsg-lib.c**：BSG 库函数，提供 `bsg_setup_queue()` 和 `bsg_remove_queue()` 供驱动注册 BSG 设备

### 14.7 disk-events.c（489 行）

文件：`block/disk-events.c`

监控磁盘事件（介质变更、弹出请求等），支持两种检测方式：

- **轮询模式**：定时轮询检测介质状态变化（`disk_events_poll_jiffies()`）
- **事件通知**：通过 sysfs 暴露 `events`、`events_async`、`events_poll_msecs` 属性

**核心数据结构**：
```c
struct disk_events {
    struct list_head    node;           // 全局 disk_events 链表
    struct gendisk      *disk;          // 关联的 gendisk
    struct mutex        block_mutex;    // 保护阻塞计数
    unsigned int        pending;        // 已发出的事件
    unsigned int        clearing;       // 正在清除的事件
    long                poll_msecs;     // 轮询间隔
    struct delayed_work dwork;          // 轮询工作项
};
```

**支持的事件类型**：
- `DISK_EVENT_MEDIA_CHANGE` — 介质变更
- `DISK_EVENT_EJECT_REQUEST` — 弹出请求

### 14.8 holder.c

文件：`block/holder.c`

管理块设备持有者关系，通过 sysfs 创建 `holders/` 和 `slaves/` 符号链接：

```c
// 例如：dm-0 映射到 sda
// /sys/block/dm-0/slaves/sda → /sys/block/sda
// /sys/block/sda/holders/dm-0 → /sys/block/dm-0

bd_link_disk_holder(bdev, disk)   // 创建持有者链接
bd_unlink_disk_holder(bdev, disk) // 移除持有者链接
```

### 14.9 early-lookup.c（316 行）

文件：`block/early-lookup.c`

在内核启动早期阶段，当根文件系统尚未挂载时，提供块设备查找功能：

- **early_lookup_bdev()**：通过设备名（如 `PARTUUID=xxx` 或 `/dev/nvme0n1p2`）查找块设备
- 支持 `PARTUUID`、`PARTLABEL`、`UUID`、`LABEL` 等多种标识符

---

## 15. 超时与电源管理

### 15.1 blk-timeout.c

文件：`block/blk-timeout.c`

实现请求超时处理机制，核心是每请求定时器和超时扫描。

#### 15.1.1 超时定时器管理

每个请求队列有一个全局定时器 `q->timeout`，管理队列中所有请求的超时：

```c
void blk_add_timer(struct request *req)
{
    // 1. 设置请求超时时间
    if (!req->timeout)
        req->timeout = q->rq_timeout;  // 默认队列超时时间
    req->rq_flags &= ~RQF_TIMED_OUT;

    // 2. 计算 deadline
    expiry = jiffies + req->timeout;
    WRITE_ONCE(req->deadline, expiry);

    // 3. 调整队列定时器
    expiry = blk_rq_timeout(blk_round_jiffies(expiry));  // 四舍五入到秒
    if (!timer_pending(&q->timeout) ||
        time_before(expiry, q->timeout.expires))
        mod_timer(&q->timeout, expiry);
}
```

**时间粒度**：`blk_round_jiffies()` 将超时时间四舍五入到最近的秒边界，用于合并定时器以减少 CPU 唤醒次数：
```c
static inline unsigned long blk_round_jiffies(unsigned long j)
{
    return (j + blk_timeout_mask) + 1;  // blk_timeout_mask = roundup_pow_of_two(HZ) - 1
}
```

#### 15.1.2 超时处理流程

```text
# 超时处理调用栈
timer_expiry → q->timeout
  │
  └─ blk_rq_timed_out_timer()
      │  # 遍历队列中所有请求，检查 deadline 是否已过期
      │
      └─ [for each request in flight]:
          │
          ├─ if time_after(jiffies, req->deadline):
          │   │  # 请求超时
          │   │
          │   └─ req->q->mq_ops->timeout(req, reserved)
          │       │  # 调用驱动层的 timeout 回调
          │       │  # 返回值决定下一步操作：
          │       │
          │       ├─ BLK_EH_DONE      → 请求已处理，无需额外操作
          │       ├─ BLK_EH_RESET_TIMER → 重置定时器，等待更长
          │       └─ BLK_EH_MULTI      → 需要多步恢复
          │
          └─ [继续检查下一个请求]
```

#### 15.1.3 主动终止请求

```c
void blk_abort_request(struct request *req)
{
    // 立即将 deadline 设为当前时间，触发超时扫描
    WRITE_ONCE(req->deadline, jiffies);
    kblockd_schedule_work(&req->q->timeout_work);
}
```

驱动可在错误恢复时调用，强制立即触发超时处理。

#### 15.1.4 I/O 失败注入

编译时通过 `CONFIG_FAIL_IO_TIMEOUT` 启用，通过 `fail_io_timeout=` 内核参数配置：

```c
static DECLARE_FAULT_ATTR(fail_io_timeout);  // 故障注入属性

bool __blk_should_fake_timeout(struct request_queue *q)
{
    return should_fail(&fail_io_timeout, 1);  // 概率性模拟超时
}
```

通过 sysfs 属性 `part_timeout_show`/`part_timeout_store` 控制 `QUEUE_FLAG_FAIL_IO` 标志，启用/禁用 I/O 失败模拟。

### 15.2 blk-pm.c / blk-pm.h

文件：`block/blk-pm.c` / `block/blk-pm.h`

实现基于请求的块设备运行时电源管理（Runtime PM）。

#### 15.2.1 初始化

```c
void blk_pm_runtime_init(struct request_queue *q, struct device *dev)
{
    q->dev = dev;
    q->rpm_status = RPM_ACTIVE;
    pm_runtime_set_autosuspend_delay(q->dev, -1);  // 初始禁止自动挂起
    pm_runtime_use_autosuspend(q->dev);             // 启用自动挂起模式
}
```

#### 15.2.2 运行时状态机

```
  RPM_ACTIVE ──blk_pre_runtime_suspend()──→ RPM_SUSPENDING ──blk_post_runtime_suspend()──→ RPM_SUSPENDED
       ↑                                                                                        │
       └───────blk_post_runtime_resume()─── RPM_RESUMING ──blk_pre_runtime_resume()──────────────┘
```

**挂起流程**（`blk_pre_runtime_suspend()` → `blk_post_runtime_suspend()`）：

```c
int blk_pre_runtime_suspend(struct request_queue *q)
{
    // 1. 设置状态为 SUSPENDING
    q->rpm_status = RPM_SUSPENDING;

    // 2. 增加 pm_only 计数器，阻止新 I/O 进入
    blk_set_pm_only(q);

    // 3. 冻结队列，等待所有进行中的 I/O 完成
    blk_freeze_queue_start(q);
    percpu_ref_switch_to_atomic_sync(&q->q_usage_counter);

    // 4. 检查是否有活跃 I/O
    if (percpu_ref_is_zero(&q->q_usage_counter))
        ret = 0;  // 可以挂起
    else
        ret = -EBUSY;  // 仍有 I/O，不能挂起

    // 5. 恢复队列（冻结只是检查）
    blk_mq_unfreeze_queue_nomemrestore(q);

    // 6. 如果检查失败，恢复状态
    if (ret < 0) {
        q->rpm_status = RPM_ACTIVE;
        pm_runtime_mark_last_busy(q->dev);
        blk_clear_pm_only(q);
    }
    return ret;
}
```

**恢复流程**（`blk_pre_runtime_resume()` → `blk_post_runtime_resume()`）：

```c
void blk_post_runtime_resume(struct request_queue *q)
{
    q->rpm_status = RPM_ACTIVE;
    pm_runtime_mark_last_busy(q->dev);
    pm_request_autosuspend(q->dev);  // 重新调度自动挂起
    blk_clear_pm_only(q);            // 清除 pm_only，允许新 I/O 进入
}
```

#### 15.2.3 请求路径中的 PM 交互

**`blk_pm.h`** 提供的内联函数：

```c
// 在请求下发时检查是否需要恢复设备
static inline int blk_pm_resume_queue(const bool pm, struct request_queue *q)
{
    if (!q->dev || !blk_queue_pm_only(q))
        return 1;  // 无需恢复
    if (pm && q->rpm_status != RPM_SUSPENDED)
        return 1;  // 请求允许（PM 请求或未挂起）
    pm_request_resume(q->dev);  // 请求恢复设备
    return 0;
}

// 在 I/O 完成后标记最后活跃时间
static inline void blk_pm_mark_last_busy(struct request *rq)
{
    if (rq->q->dev && !(rq->rq_flags & RQF_PM))
        pm_runtime_mark_last_busy(rq->q->dev);
}
```

**PM 请求标记**：通过 `RQF_PM` 标志区分：
- 普通请求：`RQF_PM` 未设置，因此在设备挂起时会被阻塞
- PM 请求：`RQF_PM` 已设置，即使在挂起状态也能下发（用于设备唤醒等关键操作）

---

## 16. 统计与跟踪

### 16.1 blk-stat.c / blk-stat.h

文件：`block/blk-stat.c` / `block/blk-stat.h`

块层统计基础设施，基于回调机制收集请求完成延迟数据。

#### 16.1.1 核心数据结构

```c
struct blk_rq_stat {
    u64 min;           // 最小值
    u64 max;           // 最大值
    u64 batch;         // 批次总和（用于计算均值）
    u64 nr_samples;    // 样本数
};

struct blk_stat_callback {
    struct list_head    list;           // 所有回调链表（RCU 保护）
    struct timer_list   timer;          // 统计周期定时器
    struct blk_rq_stat __percpu *cpu_stat; // Per-CPU 统计桶
    int (*bucket_fn)(const struct request *); // 请求分桶函数
    unsigned int        buckets;        // 桶数
    struct blk_rq_stat *stat;           // 聚合后的统计结果
    void (*timer_fn)(struct blk_stat_callback *); // 定时器回调
    void *data;                         // 私有数据
    struct rcu_head     rcu;
};

struct blk_queue_stats {
    struct list_head callbacks;  // 回调链表
    spinlock_t lock;
    int accounting;              // 基础统计计数
};
```

#### 16.1.2 统计收集流程

```text
# 请求完成时
blk_stat_add(rq, now)
  │  # 计算延迟: value = now - rq->io_start_time_ns
  │
  └─ rcu_read_lock
      └─ [遍历 q->stats->callbacks 的所有回调]:
          │
          ├─ bucket = cb->bucket_fn(rq)  # 请求分桶
          └─ stat = per_cpu_ptr(cb->cpu_stat, cpu)[bucket]
              └─ blk_rq_stat_add(stat, value)  # 更新 per-CPU 统计

# 定时器触发时
blk_stat_timer_fn(cb)
  │
  └─ [遍历所有在线 CPU]:
      │  # 聚合 per-CPU 统计到 cb->stat
      blk_rq_stat_sum(&cb->stat[bucket], &cpu_stat[bucket])
      blk_rq_stat_init(&cpu_stat[bucket])  # 重置 per-CPU 统计
      │
      └─ cb->timer_fn(cb)  # 调用用户回调

# 统计值计算
blk_rq_stat_add(stat, value):
    stat->min = min(stat->min, value)
    stat->max = max(stat->max, value)
    stat->batch += value
    stat->nr_samples++

blk_rq_stat_sum(dst, src):
    dst->min = min(dst->min, src->min)
    dst->max = max(dst->max, src->max)
    dst->mean = div_u64(src->batch + dst->mean * dst->nr_samples,
                        dst->nr_samples + src->nr_samples)
    dst->nr_samples += src->nr_samples
```

#### 16.1.3 回调生命周期管理

```c
// 分配回调
struct blk_stat_callback *
blk_stat_alloc_callback(timer_fn, bucket_fn, buckets, data);

// 添加到队列
blk_stat_add_callback(q, cb);    // 设置 QUEUE_FLAG_STATS

// 移除并释放
blk_stat_remove_callback(q, cb); // 清除 QUEUE_FLAG_STATS（若链表为空）
blk_stat_free_callback(cb);      // 通过 RCU 释放

// 激活统计窗口
blk_stat_activate_msecs(cb, msecs);  // 启动定时器
blk_stat_activate_nsecs(cb, nsecs);  // 启动定时器

// 基础统计（不注册回调，仅记录时间/大小）
blk_stat_enable_accounting(q);
blk_stat_disable_accounting(q);
```

### 16.2 块层 Tracepoints

文件：`include/trace/events/block.h`（约 684 行）

使用 ftrace 框架定义，覆盖整个块 I/O 生命周期。详见 [14.3 blktrace](#143-blktrace--块层跟踪)。

**使能方式**：
```bash
# 使用 trace-cmd
trace-cmd record -e block_rq_issue -e block_rq_complete

# 使用 perf
perf record -e block:block_rq_issue -e block:block_rq_complete

# 使用 ftrace
echo block_rq_issue > /sys/kernel/debug/tracing/set_event
```

### 16.3 blk-ioc.c（442 行）

文件：`block/blk-ioc.c`

管理 I/O 上下文（`struct io_context`），关联进程与 I/O 调度器。

#### 16.3.1 核心数据结构

```c
struct io_context {
    atomic_long_t refcount;          // 引用计数
    atomic_t active_ref;             // 活跃引用计数（进程数）
    struct hlist_head icq_list;      // I/O 上下文与队列关联（io_cq）链表
    spinlock_t lock;                 // 保护 icq_list 和 icq_tree
    int ioprio;                      // 当前 I/O 优先级
    struct radix_tree_root icq_tree; // 按队列 ID 索引的 icq 树
    struct io_cq __rcu *icq_hint;   // 最近使用的 icq 缓存（RCU）
    struct work_struct release_work; // 异步释放工作项
};

struct io_cq {
    struct request_queue *q;         // 关联的请求队列
    struct io_context *ioc;          // 关联的 io_context
    struct list_head q_node;         // 队列的 icq 链表节点
    struct hlist_node ioc_node;      // ioc 的 icq 链表节点
    unsigned int flags;              // ICQ_* 标志
};
```

#### 16.3.2 生命周期管理

```text
# icq 查找与创建
ioc_find_get_icq(q)
  │
  ├─ [ioc 不存在] alloc_io_context() → 分配新 io_context
  │
  ├─ [ioc 存在] ioc_lookup_icq(q)
  │   │  # 先查 icq_hint 缓存，再查 radix tree
  │   │
  │   └─ [icq 不存在] ioc_create_icq(q)
  │       │  # 创建 io_cq，插入 ioc 的 radix tree 和 q 的链表
  │       │  # 调用 elevator 的 init_icq 回调
  │       │
  │       └─ radix_tree_insert(&ioc->icq_tree, q->id, icq)
  │           hlist_add_head(&icq->ioc_node, &ioc->icq_list)
  │           list_add(&icq->q_node, &q->icq_list)
  │
  └─ 返回 icq

# 释放
put_io_context(ioc)
  │
  └─ [引用计数归零] ioc_delay_free(ioc)
      │
      └─ [icq_list 非空] ioc_release_fn()  → 异步释放
          [icq_list 为空] kmem_cache_free() → 直接释放
```

#### 16.3.3 进程关联

```c
// 创建 io_context（进程首次执行 I/O 时懒分配）
current->io_context = alloc_io_context(GFP_ATOMIC, node);

// 进程退出时清理
exit_io_context(task) → ioc_exit_icqs(ioc) → put_io_context(ioc)

// 子进程共享（CLONE_IO）
__copy_io(CLONE_IO, tsk)  →  tsk->io_context = current->io_context
                            atomic_inc(&ioc->active_ref)
```

### 16.4 ioprio.c（249 行）

文件：`block/ioprio.c`

实现 `ioprio_get()` 和 `ioprio_set()` 系统调用，管理 I/O 优先级。

**优先级等级**：
| 等级 | 值 | 说明 |
|------|-----|------|
| `IOPRIO_CLASS_NONE` | 0 | 未设置（继承默认） |
| `IOPRIO_CLASS_RT` | 1 | 实时（需要 CAP_SYS_ADMIN 或 CAP_SYS_NICE） |
| `IOPRIO_CLASS_BE` | 2 | 尽力而为（默认） |
| `IOPRIO_CLASS_IDLE` | 3 | 空闲（仅在磁盘空闲时运行） |

**优先级编码**：`class << IOPRIO_CLASS_SHIFT | level`（level 0-7，0 最高）

**权限检查**（`ioprio_check_cap()`）：
- `IOPRIO_CLASS_RT`：需要 `CAP_SYS_ADMIN` 或 `CAP_SYS_NICE`
- `IOPRIO_CLASS_IDLE`：不需要特殊权限
- `IOPRIO_CLASS_NONE`：level 必须为 0

### 16.5 blk-ia-ranges.c（314 行）

文件：`block/blk-ia-ranges.c`

管理独立访问范围（Independent Access Ranges），通过 sysfs 导出每个范围的信息。

**sysfs 路径**：`/sys/block/<disk>/independent_access_ranges/`

每个范围目录包含：

| 属性 | 说明 |
|------|------|
| `sector` | 起始扇区 |
| `nr_sectors` | 扇区数 |

**核心数据结构**：
```c
struct blk_independent_access_range {
    struct kobject kobj;       // sysfs kobject
    sector_t sector;           // 起始扇区
    sector_t nr_sectors;       // 扇区数
};

struct blk_independent_access_ranges {
    struct kobject kobj;                     // sysfs kobject（父目录）
    unsigned int nr_ia_ranges;                // 范围数
    struct blk_independent_access_range ia_range[]; // 柔性数组
};
```

### 16.6 badblocks.c（1,550 行）

文件：`block/badblocks.c`

管理坏块记录，支持设置/清除坏块范围。

**记录格式**：每个坏块记录为 `(sector, count, acked)` 三元组，以有序数组存储在 `struct badblocks` 中：

```c
struct badblocks {
    seqlock_t lock;       // 顺序锁（允许读-写并发）
    int count;            // 记录数
    int shift;            // 粒度移位（2^shift 扇区为单位）
    u64 *page;            // 记录数组（每个 64 位）
    int length;           // 数组长度（页对齐）
    int changed;          // 是否已更改（sysfs 通知用）
};
```

**核心操作**：
- `badblocks_set(bb, sector, count, acked)` — 设置坏块范围
- `badblocks_clear(bb, sector, count)` — 清除坏块范围
- `badblocks_check(bb, sector, count, ...)` — 检查扇区范围是否包含坏块

**合并策略**：处理 6 种重叠情况（不相邻、S 包含 E、E 包含 S、S 在 E 前、S 在 E 后、完全覆盖），支持 acked/unacked 状态转换。

---