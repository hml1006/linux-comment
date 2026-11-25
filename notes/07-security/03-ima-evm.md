# IMA / EVM 完整性度量与保护

> 源码基线 Linux 7.2。相关代码位于 `security/integrity/`，包括 IMA（`ima/`）、EVM（`evm/`）、数字签名验证（`digsig.c`、`digsig_asymmetric.c`）和共享的 `iint.c`/`integrity.h`。
>
> **配置前提**：当前 `.config` 中 `CONFIG_SECURITY` 未启用，而 `CONFIG_IMA`/`CONFIG_EVM` 都依赖 `CONFIG_INTEGRITY` 且 IMA/EVM 自身是 LSM（`DEFINE_LSM(ima)`、`DEFINE_LSM(evm)`，均 `LSM_ORDER_LAST`）。同时 `CONFIG_KEYS`、`CONFIG_CRYPTO` 也未启用，而 IMA `select CRYPTO/CRYPTO_HMAC/CRYPTO_SHA1`、EVM `select KEYS/ENCRYPTED_KEYS`。因此本构建中整个 `security/integrity/` 不参与编译。下文描述源码中完整存在的机制。

## 1. 三个子功能

IMA（Integrity Measurement Architecture）提供三件事：

| 功能 | 含义 | 失败后果 |
|------|------|----------|
| **measure（度量）** | 对文件读取/执行/模块加载等事件计算哈希，追加到测量日志，并扩展到 TPM PCR | 无（只记录） |
| **appraise（评估）** | 把文件实际哈希/签名与 `security.ima` xattr 中存的值比对 | `-EACCES`（enforcing） |
| **audit（审计）** | 把度量/评估结果写入 audit 日志 | 无 |

EVM（Extended Verification Module）保护**元数据**：对一组安全 xattr（`security.selinux`、`security.SMACK64`、`security.ima`、`security.capability`、`security.apparmor` 等）整体计算 HMAC 或签名，存进 `security.evm`。IMA 保护文件内容，EVM 保护“描述文件内容的那些标签”，两者配合才能抵御离线篡改。

```
文件读取/执行
   │  security_file_mmap / bprm_check / file_check ...
   ▼
process_measurement()                 ima_main.c
   ├─ ima_get_action() ── ima_match_policy() ── ima_rule_entry[]（策略）
   ├─ measure: ima_collect_measurement() → 哈希 → ima_store_measurement()
   │             └─ ima_add_template_entry() → ima_measurements 链表 + TPM extend
   ├─ appraise: ima_appraise_measurement()
   │             ├─ ima_read_xattr() 读 security.ima
   │             ├─ xattr_verify() 比对哈希 或 integrity_digsig_verify() 验签
   │             └─ evm_verifyxattr() 校验 security.evm
   └─ audit: ima_audit_measurement() / integrity_audit_msg()
```

## 2. 配置项

| 配置 | 作用 |
|------|------|
| `CONFIG_IMA` | IMA 主体，select `SECURITYFS/CRYPTO/CRYPTO_HMAC/CRYPTO_SHA1/CRYPTO_HASH_INFO/SECURITY_PATH`、`TCG_TPM`（有 IOMEM 时） |
| `CONFIG_IMA_APPRAISE` | 启用本地评估（需要 `security.ima` 标签） |
| `CONFIG_IMA_APPRAISE_BOOTPARAM` | 允许 `ima_appraise=fix\|log\|enforce` 命令行 |
| `CONFIG_IMA_MEASURE_PCR_IDX` | 默认 PCR（代码中 `pcr = CONFIG_IMA_MEASURE_PCR_IDX`） |
| `CONFIG_IMA_DEFAULT_HASH` / `CONFIG_IMA_DEFAULT_TEMPLATE` | 默认哈希算法与模板名（`ima-ng` 等） |
| `CONFIG_IMA_QUEUE_EARLY_BOOT_KEYS` | 早期启动密钥延后度量 |
| `CONFIG_EVM` | EVM，select `KEYS/ENCRYPTED_KEYS/CRYPTO_HMAC/CRYPTO_SHA1/CRYPTO_HASH_INFO/SECURITY_PATH` |
| `CONFIG_EVM_ATTR_FSUUID` | HMAC 输入包含 fs UUID（`evm_hmac_attrs |= EVM_ATTR_FSUUID`） |
| `CONFIG_EVM_EXTRA_SMACK_XATTRS` | 额外保护 Smack 的 exec/transmute/mmap xattr |
| `CONFIG_EVM_LOAD_X509` / `CONFIG_EVM_X509_PATH` | 支持 X.509 签名型 `security.evm` |
| `CONFIG_INTEGRITY_ASYMMETRIC_KEYS` / `CONFIG_INTEGRITY_PLATFORM_KEYRING` | 非对称验签与平台密钥环 |

## 3. 核心数据结构

### 3.1 评估/度量状态缓存 `ima_iint_cache`

```c
// security/integrity/ima/ima.h
struct ima_iint_cache {
	struct mutex mutex;              /* 保护 version/flags/digest */
	struct integrity_inode_attributes real_inode;
	unsigned long flags;             /* IMA_MEASURE/IMA_APPRAISE/IMA_DIGSIG ... */
	unsigned long measured_pcrs;
	unsigned long atomic_flags;      /* IMA_CHANGE_ATTR / IMA_CHANGE_XATTR ... */
	enum integrity_status ima_file_status:4;
	enum integrity_status ima_mmap_status:4;
	enum integrity_status ima_bprm_status:4;
	enum integrity_status ima_read_status:4;
	enum integrity_status ima_creds_status:4;
	struct ima_digest_data *ima_hash;
};
```

它不放在 inode 里，而是通过 inode 的 LSM blob 存一个指针：

```c
static inline struct ima_iint_cache *
ima_inode_get_iint(const struct inode *inode)
{
	struct ima_iint_cache **iint_sec = inode->i_security + ima_blob_sizes.lbs_inode;
	return *iint_sec;
}
```

`ima_blob_sizes.lbs_inode = sizeof(struct ima_iint_cache *)`。全局哈希表 `ima_htable`（`ima_queue.c`）以 `(inode, version)` 为键去重，避免同一文件重复度量。

`enum integrity_status`（`include/linux/integrity.h`）：`INTEGRITY_PASS`、`PASS_IMMUTABLE`、`FAIL`、`FAIL_IMMUTABLE`、`NOLABEL`、`NOXATTRS`、`UNKNOWN`。

iint 的分配与释放在 `ima_iint.c` 中实现，构成完整生命周期：

```c
// security/integrity/ima/ima_iint.c:91
struct ima_iint_cache *ima_inode_get(struct inode *inode)
{
	struct ima_iint_cache *iint;

	iint = ima_iint_find(inode);
	if (iint)
		return iint;

	iint = kmem_cache_alloc(ima_iint_cache, GFP_NOFS);
	if (!iint)
		return NULL;

	ima_iint_init_always(iint, inode);

	inode->i_flags |= S_IMA;
	ima_inode_set_iint(inode, iint);

	return iint;
}
```

- 专用 slab `ima_iint_cache` 由 `ima_iintcache_init()` 以 `SLAB_PANIC` 标志创建——启动期创建失败即致命错误；
- `ima_iint_find()` 先检查 `IS_IMA(inode)`（即 `S_IMA` 标志），避免读取未初始化的 blob；
- inode 释放时由 `inode_free_security_rcu` hook 调用 `ima_inode_free_rcu()`，释放 `ima_hash` 并把 iint 归还 slab；
- `ima_iint_init_always()` 把五个状态字段初始化为 `INTEGRITY_UNKNOWN`，并按 `s_stack_depth` 对 `iint->mutex` 做 lockdep 类标注（`IMA_MAX_NESTING = FILESYSTEM_MAX_STACK_DEPTH + 1`），避免 overlayfs 与底层文件系统嵌套加锁的误报。

iint 是**惰性分配**的：只有真正命中策略的 inode 才会获得 iint，内存开销与策略覆盖的文件数成正比，而不是与系统 inode 总数成正比。LSM blob 机制本身见 [01-lsm.md](01-lsm.md)。

### 3.2 策略规则 `ima_rule_entry`

```c
// security/integrity/ima/ima_policy.c
struct ima_rule_entry {
	struct list_head list;
	int action;            /* MEASURE / DONT_MEASURE / APPRAISE / DONT_APPRAISE / AUDIT */
	unsigned int flags;    /* 哪些字段有效：IMA_FUNC/IMA_MASK/IMA_UID/IMA_FSMAGIC/... */
	enum ima_hooks func;   /* FILE_CHECK / MMAP_CHECK / BPRM_CHECK / MODULE_CHECK ... */
	int mask;
	unsigned long fsmagic;
	uuid_t fsuuid;
	kuid_t uid;  kgid_t gid;
	kuid_t fowner; kgid_t fgroup;
	bool (*uid_op)(...); /* lt/le/eq/ge/gt 运算符 */
	...
	int pcr;
	unsigned int allowed_algos;   /* 允许的哈希算法位图 */
	struct { void *rule; char *args_p; int type; } lsm[MAX_LSM_RULES];
	char *fsname, *fs_subtype;
	struct ima_rule_opt_list *keyrings;
	struct ima_rule_opt_list *label;
	struct ima_template_desc *template;
};
```

`enum ima_hooks` 由 `__ima_hooks()` X-macro 生成（`ima.h:378`），与 LSM hook 一一对应：`FILE_CHECK`、`MMAP_CHECK`、`BPRM_CHECK`、`MODULE_CHECK`、`FIRMWARE_CHECK`、`KEXEC_KERNEL_CHECK`、`POLICY_CHECK`、`CRITICAL_DATA` 等。

### 3.3 模板与测量条目

```c
struct ima_template_desc {
	struct list_head list;
	char *name;                 /* "ima-ng"/"ima-sig"/"ima-ngv2"/"ima-sigv2"/"ima-buf" */
	char *fmt;                  /* "d-ng|n-ng|sig" */
	int num_fields;
	const struct ima_template_field **fields;
};

struct ima_template_entry {
	int pcr;
	struct tpm_digest *digests;
	struct ima_template_desc *template_desc;
	u32 template_data_len;
	struct ima_field_data template_data[];  /* 变长字段区 */
};
```

模板在 `ima_template.c:19` 的 `builtin_templates[]` 中定义。字段实现（`d-ng` 摘要、`n-ng` 文件名、`sig` 签名、`buf` 数据）在 `ima_template_lib.c`，每个字段注册 `field_init`/`field_show`。

## 4. 度量路径

### 4.1 入口

IMA 作为 LSM 注册（`ima_main.c`）：

```c
struct lsm_blob_sizes ima_blob_sizes = { .lbs_inode = sizeof(struct ima_iint_cache *) };

DEFINE_LSM(ima) = {
	.id = &ima_lsmid,             /* name = "ima", id = LSM_ID_IMA */
	.init = init_ima_lsm,
	.order = LSM_ORDER_LAST,
	.blobs = &ima_blob_sizes,
	.initcall_late = init_ima,    /* TPM 可用后再启动 */
};
```

hook 列表覆盖 `file_mmap`、`file_mprotect`、`file_check`、`bprm_check_security`、`kernel_read_file`/`kernel_post_read_file`、`kernel_load_data`、`inode_setxattr`、`inode_removexattr`、`file_truncate`、`path_post_mknod`、`inode_free_security_rcu`、`key_post_create_or_update`、`kernel_module_request`。所有 hook 最终汇入 `process_measurement()`。

### 4.2 `process_measurement()`

```c
static int process_measurement(struct file *file, const struct cred *cred,
			       struct lsm_prop *prop, char *buf, loff_t size,
			       int mask, enum ima_hooks func,
			       enum kernel_read_file_id read_id,
			       bool bprm_is_check)
```

关键步骤：

1. `if (!ima_policy_flag || !S_ISREG(inode->i_mode)) return 0;` —— 只处理普通文件，且策略未启用时零开销；
2. `ima_get_action()` → `ima_match_policy()`：按 `mask`、`func`、uid/gid、fsmagic、LSM 规则等算出 `action` 位图（`IMA_MEASURE`、`IMA_APPRAISE`、`IMA_AUDIT`）并返回目标 `pcr` 与模板；
3. `violation_check`：对 `FILE_CHECK`/`MMAP_CHECK`，若策略要求度量但文件以写方式打开，记录“先写后读”违规（`ima_rdwr_violation_check()`）；
4. `ima_inode_get(inode)` 取/建 `iint`，按 `IMA_CHANGE_ATTR`/`IMA_CHANGE_XATTR` 清除缓存；对 stacked 文件系统用 `d_real_inode()` 检测底层变化；
5. `iint->flags |= action; action &= IMA_DO_MASK;` 去掉已完成的部分（`IMA_MEASURED`/`IMA_APPRAISED`）；
6. 度量：`ima_collect_measurement(iint, file, buf, size, algo, modsig)` 计算文件哈希（`ima_calc_file_hash()`，大文件分块），随后 `ima_store_measurement()`；
7. 评估：`ima_appraise_measurement()`（见第 5 节）；
8. 审计：`ima_audit_measurement()`。

逐行阅读 `process_measurement()`（`ima_main.c:254`）还能看到几个容易忽略的设计点：

**动作位运算。** IMA 把“要做”与“已做”编码在相邻位上（`IMA_MEASURE 0x1`/`IMA_MEASURED 0x2`，`ima.h:142`）：

```c
// ima_main.c:371
iint->flags |= action;
action &= IMA_DO_MASK;
action &= ~((iint->flags & (IMA_DONE_MASK ^ IMA_MEASURED)) >> 1);

/* If target pcr is already measured, unset IMA_MEASURE action */
if ((action & IMA_MEASURE) && (iint->measured_pcrs & (0x1 << pcr)))
	action ^= IMA_MEASURE;
```

把 DONE 位右移一位再取反，就得到“尚未完成的动作”；`measured_pcrs` 按 PCR 记录，因此同一文件可以分别度量到不同 PCR 各一次。

**文件变化检测。** `ima_detect_file_change()`（`ima_main.c:188`）优先用 `vfs_getattr_nosec()` 取 `STATX_CHANGE_COOKIE`，不支持时退回 `i_version`，两者都没有则保守地认为文件已变化。写侧由 `ima_file_free()`（`file_free` hook，`__fput()` 时调用）→ `ima_check_last_writer()`：最后一个写者关闭文件（`i_writecount == 1`）时，若文件有变化就清掉 `IMA_DONE_MASK` 与 `measured_pcrs`，并在 `IMA_UPDATE_XATTR` 置位时调用 `ima_update_xattr()` 重写 `security.ima` 哈希，保证下一次读取重新评估新内容。

**其它边界行为**：

| 场景 | 行为 |
|------|------|
| `IMA_FILE_APPRAISE` 规则 | 把 `func` 归一为 `FILE_CHECK`，共用文件状态缓存 |
| `SB_I_IMA_UNVERIFIABLE_SIGNATURE` 文件系统 | 除非可信挂载且策略未要求 `IMA_FAIL_UNVERIFIABLE_SIGS`，否则清 DONE 位强制重评 |
| `READING_MODULE_COMPRESSED` | 压缩模块的评估推迟到解压后的 `READING_MODULE` 阶段 |
| `allowed_algos` 不匹配 | 返回 `-EACCES` 并审计 `denied-hash-algorithm` |
| `O_DIRECT` + `IMA_PERMIT_DIRECTIO` | 清零错误码，允许直接 I/O 绕过哈希失败 |
| 写打开已签名文件（`MAY_WRITE` + `IMA_DIGSIG`） | 直接 `-EACCES`：签名文件不允许被写 |
| `must_appraise` 且 `IMA_APPRAISE_ENFORCE` | 最终统一返回 `-EACCES`；否则返回 0 只记录 |

### 4.3 测量日志与 TPM

`ima_store_measurement()` → `ima_alloc_init_template()`（按 `template_desc` 逐字段填充）→ `ima_add_template_entry()`（`ima_queue.c:242`）：

- 若存在 TPM（`ima_tpm_chip`），对模板数据做 `tpm_pcr_extend()` 扩展到对应 PCR；
- boot aggregate 由 `ima_calc_boot_aggregate()` 计算：对 PCR 0-7 做累积摘要，非 SHA1 bank 还会把 PCR 8-9 计入；
- `ima_add_digest_entry()` 用 `(inode, version)` 查 `ima_htable` 去重，然后挂到 `ima_measurements` 链表；
- 支持 staged 列表（`ima_measurements_staged`）以便 kexec 迁移时整体替换；
- 测量日志可通过 `securityfs` 读取：`/sys/kernel/security/ima/ascii_runtime_measurements`、`binary_runtime_measurements`、`runtime_measurements_count`、`violations`（`ima_fs.c:773` 的 `ima_fs_init()`）。

启动时 `init_ima()`（`ima_init.c`）依次：`integrity_init_keyring(INTEGRITY_KEYRING_IMA)` → `ima_init_crypto()` → `ima_init_template()` → `ima_init_digests()` → `ima_init_htable()` → `ima_add_boot_aggregate()`（boot aggregate 必须作为第一条记录，聚合 TPM 0-7 PCR 或使用 `boot_aggregate` 模板）→ `ima_init_policy()` → `ima_fs_init()`。无 TPM 时打印 `No TPM chip found, activating TPM-bypass!`，度量仍记录但不 extend。

深入 `ima_queue.c` 可以看到，测量日志是**只追加**的（文件头注释明确 “No entry is ever removed or changed during the boot-cycle”），并发模型如下：

- `ima_extend_list_mutex` 保护“追加 + TPM extend”的原子性；TPM 操作可能很慢，所以用 mutex 而非 spinlock；
- 读侧（securityfs dump、`ima_lookup_digest_entry()`）走 RCU；
- reboot notifier `ima_reboot_notifier()` 在重启前置位 `ima_measurements_suspended`，此后追加返回 `-ENODEV`（审计 cause `measurements_suspended`），避免向已关闭的 TPM 写入；kexec 重启则先度量一条 `kexec_execute` 事件。

**去重键是摘要而不是 inode。** `ima_lookup_digest_entry()`（`ima_queue.c:107`）用 `ima_hash_key()`（取摘要前两字节模 `IMA_MEASURE_HTABLE_SIZE`，`ima.h:348`）定位桶，再 `memcmp()` 比较 `digests[ima_hash_algo_idx]` 与 `pcr`，命中即返回 `-EEXIST`（审计 `hash_exists`）。因此同一文件在不同文件系统上的两份拷贝只度量一次；iint 里的 `(inode, version)` 信息负责判断“是否需要重新哈希”，htable 负责“日志里是否已有”。`CONFIG_IMA_DISABLE_HTABLE` 可关闭去重（每次度量都记录），`ima_flush_htable` 启动参数则在清空 staged 列表时一并重建哈希表。

**违例条目使 PCR 失效。** `ima_add_violation()`（`ima_api.c:135`）在日志里存全零摘要，但实际 extend 到 PCR 的是预分配的全 `0xFF` `digests` 数组（`ima_init_digests()`，`ima_queue.c:550`）——验证者无法从日志复现全 FF 的 extend，PCR 即被视为不可信。

**staged 列表管理。** `ima_queue_stage()` 把 `ima_measurements` 整体移入 `ima_measurements_staged`（`list_replace`），`ima_queue_staged_delete_all()` 清空 staged 列表，`ima_queue_delete_partial()` 从当前列表头部裁剪指定条数；三者都拒绝在 suspend 之后操作，保证 kexec dump 可以无锁读取。释放条目时 `ima_queue_delete()` 只释放 `template_data` 而保留 `digests`（仍被 htable 引用），除非同时 flush 哈希表。

### 4.4 哈希计算与多 bank 摘要（`ima_crypto.c`）

IMA 只使用同步 shash 前端（Crypto API 分层见 [04-crypto-keyring.md](04-crypto-keyring.md)）：

```c
// ima_crypto.c:193 ima_calc_file_hash_tfm() 核心循环
rbuf = kzalloc(PAGE_SIZE, GFP_KERNEL);
while (offset < i_size) {
	rbuf_len = integrity_kernel_read(file, offset, rbuf, PAGE_SIZE);
	if (rbuf_len < 0) { rc = rbuf_len; break; }
	if (rbuf_len == 0) { rc = -EINVAL; break; }	/* unexpected EOF */
	offset += rbuf_len;
	rc = crypto_shash_update(shash, rbuf, rbuf_len);
	if (rc) break;
}
```

- 默认算法的 tfm（`ima_shash_tfm`）在启动时由 `ima_init_crypto()` 分配；其它算法先查 `ima_algo_array[]`（每个 TPM bank 一个槽位，外加 `ima_extra_slots`），查不到才临时分配、用完释放；
- `ima_calc_file_hash()` 对 `O_DIRECT` 文件直接返回 `-EINVAL`；对只写打开的文件用 `dentry_open()` 重开一个只读实例来读内容；
- 缓冲区哈希（`ima_calc_buffer_hash()`，用于模块/固件/kexec cmdline 等）同样按 `PAGE_SIZE` 分块 update。

**模板摘要按 bank 计算。** `ima_calc_field_array_hash()`（`ima_crypto.c:335`）先算 SHA1 摘要（`ima_sha1_idx` 槽位恒存在），再对每个 TPM bank 各算一遍；内核不认识的 bank 算法用 SHA1 摘要填充。非 `ima` 模板的每个字段带 4 字节长度前缀参与哈希（`ima_canonical_fmt` 时为小端）； legacy `ima` 模板的文件名字段则填充到固定 `IMA_EVENT_NAME_LEN_MAX + 1` 长度——这是两种模板格式兼容性差异的来源。

**boot aggregate 的 bank 选择。** `ima_calc_boot_aggregate()` 优先选与默认算法一致的 bank，其次 SHA256，再次 SHA1；`ima_calc_boot_aggregate_tfm()` 对 PCR 0–7 做累积哈希，非 SHA1 bank 还额外计入 PCR 8–9（命令行与内核镜像），避免 SHA1 下 PCR 8/9 用途的歧义。

## 5. 评估（appraisal）路径

`ima_appraise_measurement(func, iint, file, filename, xattr_value, xattr_len, modsig, ...)`（`ima_appraise.c:462`）：

1. `iint->ima_hash` 已有则复用，否则 `ima_collect_measurement()`；
2. 若无 `security.ima` xattr：状态 `INTEGRITY_NOXATTRS`；若策略要求签名（`IMA_DIGSIG_REQUIRED`）则失败；
3. 有 xattr 则 `xattr_verify()`：
   - `IMA_XATTR_DIGEST` / `IMA_XATTR_DIGEST_NG`：`memcmp()` 比对哈希；`DIGEST_NG` 首字节是算法 ID。要求签名但只有哈希 → `IMA-signature-required`；
   - `EVM_IMA_XATTR_DIGSIG`：解析 `struct signature_v2_hdr`（版本不超过 3），`integrity_digsig_verify(INTEGRITY_KEYRING_IMA, ...)` 用 IMA 密钥环验签；返回 `-EOPNOTSUPP` 视为 `INTEGRITY_UNKNOWN`；`KEXEC_KERNEL_CHECK` 可回退到 `INTEGRITY_KEYRING_PLATFORM`；
   - `IMA_VERITY_DIGSIG`：fs-verity 场景；
4. `evm_verifyxattr()` 校验 `security.evm`；
5. 根据 `ima_appraise` 模式决定行为：`IMA_APPRAISE_ENFORCE` 返回 `-EACCES`，`IMA_APPRAISE_FIX` 尝试修复（重写 xattr），`IMA_APPRAISE_LOG` 只记录。

`ima_appraise=` 解析在 `ima_appraise.c:29` 附近（`core_param(ima_appraise, ...)`），需要 `CONFIG_IMA_APPRAISE_BOOTPARAM`。模式位定义在 `ima.h:471`：`IMA_APPRAISE_ENFORCE 0x01`、`FIX 0x02`、`LOG 0x04`、`MODULES 0x08`、`FIRMWARE 0x10`、`POLICY 0x20`、`KEXEC 0x40`。

xattr 写入路径由 `ima_inode_setxattr()`/`ima_inode_removexattr()` 守门：非特权用户不能随意写 `security.ima`；`security_inode_init_security` 期间由 `ima_inode_init_security()` 生成初始哈希。

### 5.1 xattr 类型推断与旧格式兼容

`ima_get_hash_algo()`（`ima_appraise.c:184`）从 xattr 本身推断哈希算法，保证“验签用的算法与签名时一致”：

| xattr 类型 | 推断规则 |
|-----------|----------|
| `IMA_XATTR_DIGEST_NG` | `data[0]` 即算法 ID |
| `IMA_XATTR_DIGEST` | 长度 21：末 4 字节为零 → MD5，否则 SHA1；长度 17 → MD5（历史遗留） |
| `EVM_IMA_XATTR_DIGSIG` | `signature_v2_hdr.hash_algo`（版本须为 2 或 3） |
| `IMA_VERITY_DIGSIG` | `signature_v2_hdr.hash_algo`（版本须为 3） |

`xattr_verify()`（`ima_appraise.c:250`）的边界检查值得留意——源码注释明确说明用加法而非减法，防止过短的 `xattr_len` 在无符号运算下回绕绕过检查：

```c
if (xattr_len >= (int)sizeof(xattr_value->type) + hash_start +
		(int)iint->ima_hash->length)
	rc = memcmp(&xattr_value->data[hash_start],
		    iint->ima_hash->digest,
		    iint->ima_hash->length);
else
	rc = -EINVAL;
```

**新文件豁免**：无 xattr 且 `FMODE_CREATED`/`IMA_NEW_FILE` 置位时，若不要求签名（或文件长度为 0），状态直接给 `INTEGRITY_PASS`——否则每个新建文件都会评估失败。`bprm_is_check` 为真（`AT_EXECVE_CHECK` 场景）时审计记录类型改为 `AUDIT_INTEGRITY_USERSPACE`，区分“内核强制”与“用户态解释器自查”。

### 5.2 写保护与算法白名单

`ima_protect_xattr()`（`ima_appraise.c:662`）要求写 `security.ima` 必须 `CAP_SYS_ADMIN`；`ima_inode_setxattr()` 还会调用 `validate_hash_algo()`：

- 若策略含 `SETXATTR_CHECK` 规则（`ima_setxattr_allowed_hash_algorithms` 原子变量非零），只允许白名单内算法，违者审计 `denied-hash-algorithm`；
- 否则允许默认算法，或 `crypto_has_alg()` 确认内核内置实现的任意算法；违者审计 `unavailable-hash-algorithm`。

这防止用户写入一个内核无法计算的算法的 `security.ima`，导致后续评估永远卡在 `UNKNOWN`。fix 模式下 `ima_fix_xattr()`（`ima_appraise.c:88`）拒绝在只读/immutable 文件上写入，按算法选择 `IMA_XATTR_DIGEST`（SHA1/MD5）或 `IMA_XATTR_DIGEST_NG`，且绝不覆盖已有签名。`ima_inode_post_setattr()` 在元数据变化（chmod/chown 等）时置 `IMA_CHANGE_ATTR`，使下一次 `process_measurement()` 清掉评估缓存重新评估。

## 6. IMA 策略

`/sys/kernel/security/ima/policy` 接收文本规则，`ima_parse_add_rule()` 解析（`ima_policy.c`），规则形如：

```
measure func=FILE_CHECK mask=MAY_READ uid=0
appraise func=BPRM_CHECK mask=MAY_EXEC
measure func=MODULE_CHECK template=ima-sig
audit func=FILE_CHECK mask=MAY_WRITE
dont_measure fsmagic=0x9fa0
measure func=KEY_CHECK keyrings=.ima
```

语法要素：`action`（`measure`/`dont_measure`/`appraise`/`dont_appraise`/`audit`）、`func=`、`mask=`、`fsmagic=`、`fsname=`、`fsuuid=`、`uid/gid/fowner/fgroup`（支持 `< <= = >= >`）、`obj_type=`、`lsm=`（如 `lsm=selinux`）、`pcr=`、`template=`、`digest_type=`、`keyrings=`、`label=`、`appraise_type=imasig|modsig|veritysig`、`appraise_flag=check_blacklist`、`permit_directio`。

内置策略由 `ima_policy.c` 的 `dont_measure_rules[]`、`original_measurement_rules[]`、`default_measurement_rules[]`、`default_appraise_rules[]` 等数组组成，通过 `ima_tcb`、`ima_appraise_tcb`、`ima_policy=tcb|appraise_tcb|secure_boot|...` 选择（`__setup` 在 `ima_policy.c:279/308/315`）。策略一旦写入并 `ima_update_policy()` 即锁定，可用 `ima_delete_rules()` 在测试场景清除。

审计由 `integrity_audit_msg()`（`integrity_audit.c`）输出 `audit: type=1800 ... cause=...` 形式记录，包含 pid、comm、name、dev、ino、res、cause 等字段。

### 6.1 匹配算法：RCU + 动作掩码

`ima_match_policy()`（`ima_policy.c:752`）在 RCU 读锁下遍历规则链表（策略读远多于写，是经典 RCU 场景）。核心是 `actmask` 技巧：

```c
int action = 0, actmask = flags | (flags << 1);
...
if (!(entry->action & actmask))
	continue;
...
if (entry->action & IMA_DO_MASK)
	actmask &= ~(entry->action | entry->action << 1);
else
	actmask &= ~(entry->action | entry->action >> 1);
...
if (!actmask)
	break;
```

`actmask` 同时覆盖“要做”与“已做”位：一旦某条 `measure` 规则命中，MEASURE 与 MEASURED 一起从掩码中移除，后面的 `dont_measure` 规则就无法再推翻它——**首条匹配规则定案**，所有请求的动作都有结论后提前结束遍历。`dont_*` 规则则清除对应动作位。appraise 命中时 `get_subaction()` 再细分出 `IMA_FILE_APPRAISE`/`IMA_BPRM_APPRAISE` 等按 hook 的子动作，并清掉 `IMA_HASH`。

`ima_update_policy_flags()`（`ima_policy.c:824`）在策略更新后把所有规则的动作汇总成全局 `ima_policy_flag`——这正是各 hook 入口“策略未启用零开销”快速路径的来源；`SETXATTR_CHECK` 规则不参与汇总，用 `atomic_cmpxchg` 写入以保证只有一条生效。

### 6.2 策略加载协议

`policy` 文件的 open/write/close 三部曲（`ima_fs.c`）实现了“单写者、关闭时提交”的协议：

1. `ima_open_policy()`（`ima_fs.c:706`）：`test_and_set_bit(IMA_FS_BUSY)` 保证同一时刻只有一个写者，否则 `-EBUSY`；读需要 `CONFIG_IMA_READ_POLICY` 且 `CAP_SYS_ADMIN`；
2. `ima_write_policy()`（`ima_fs.c:566`）：拒绝非零偏移（不允许部分写），单次写入上限 `PAGE_SIZE - 1`；内容以 `/` 开头则视为路径，走 `ima_read_policy()` 加载策略文件（该文件本身可被 IMA 评估）；若启用 `IMA_APPRAISE_POLICY`，直接写规则会被拒绝，必须使用签名策略文件；否则 `ima_parse_add_rule()` 逐条解析；
3. `ima_release_policy()`（`ima_fs.c:731`）：关闭时 `ima_check_policy()` 校验，成功则 `ima_update_policy()` 切换 `ima_rules` 指针（RCU 发布）并更新 `ima_policy_flag`；失败则 `ima_delete_rules()` 回滚本次写入的规则。未启用 `CONFIG_IMA_WRITE_POLICY`/`CONFIG_IMA_READ_POLICY` 时，策略文件在成功加载后**被直接移除**——策略就此锁定一个启动周期。

这解释了为什么部署时通常用一次 `cat policy > /sys/kernel/security/ima/policy` 写完所有规则再关闭文件：提交点是 `release` 而不是 `write`。

## 7. EVM

### 7.1 受保护的 xattr

`evm_main.c:38` 的 `evm_config_default_xattrnames[]` 按编译配置确定：

| xattr | 启用条件 |
|-------|----------|
| `security.selinux` | `CONFIG_SECURITY_SELINUX` |
| `security.SMACK64` | `CONFIG_SECURITY_SMACK` |
| `security.SMACK64EXEC/TRANSMUTE/MMAP` | `CONFIG_EVM_EXTRA_SMACK_XATTRS` |
| `security.apparmor` | `CONFIG_SECURITY_APPARMOR` |
| `security.ima` | `CONFIG_IMA_APPRAISE` |
| `security.capability` | 始终 |

可通过 `/sys/kernel/security/evm/evm_xattrs` 增删受保护项（`evm_secfs.c:292`），`/sys/kernel/security/evm` 文件用于加载 EVM 密钥（`evm_key_ops`）。

### 7.2 `security.evm` 的两种形态

`enum evm_ima_xattr_type`（`security/integrity/integrity.h:24`）：

- `EVM_XATTR_HMAC`：`struct evm_xattr { u8 type; u8 digest[SHA1_DIGEST_SIZE]; }`，HMAC-SHA1，密钥来自 `INTEGRITY_KEYRING_EVM`，不可移植（受 fs UUID 影响，若启用 `CONFIG_EVM_ATTR_FSUUID`）；
- `EVM_XATTR_PORTABLE_DIGSIG` / `EVM_IMA_XATTR_DIGSIG`：`struct signature_v2_hdr` + 签名，可移植（portable 表示不绑定本机 UUID），验签走 EVM 密钥环；X509 模式需要 `EVM_INIT_X509`。

### 7.3 验证流程 `evm_verify_hmac()`

```c
static enum integrity_status evm_verify_hmac(struct dentry *dentry,
					     const char *xattr_name,
					     char *xattr_value,
					     size_t xattr_value_len)
```

1. 若 `iint->evm_status` 已是 `INTEGRITY_PASS`/`PASS_IMMUTABLE`，直接返回（缓存）；
2. 不支持 HMAC 的文件系统（`SB_I_EVM_HMAC_UNSUPPORTED`）返回 `INTEGRITY_UNKNOWN`；
3. `vfs_getxattr_alloc(XATTR_NAME_EVM)` 读 `security.evm`：
   - `-ENODATA` 且存在其它受保护 xattr → `INTEGRITY_NOLABEL`；不存在任何受保护 xattr → `INTEGRITY_NOXATTRS`（新文件）；
   - `-EOPNOTSUPP` → `INTEGRITY_UNKNOWN`；
4. 按 `xattr_data->type` 分支：
   - HMAC：`evm_calc_hmac()` 重算，`crypto_memneq()` 常数时间比较；
   - DIGSIG/PORTABLE_DIGSIG：`evm_calc_hash()` 得到受保护 xattr 集合的摘要，`integrity_digsig_verify(INTEGRITY_KEYRING_EVM, ...)` 验签；`PORTABLE_DIGSIG` 标记 `EVM_IMMUTABLE_DIGSIG` 且状态为 `PASS_IMMUTABLE`；对可写 inode，验签通过后 `evm_update_evmxattr()` 刷新 HMAC。

### 7.4 更新与保护

- `evm_protect_xattr()`（`evm_main.c:520`）是 `inode_setxattr`/`inode_removexattr` 的核心：要修改某个受保护 xattr 时，先计算/验证 `security.evm`；若校验失败则拒绝，若通过则允许并随后重写 `security.evm`。
- `evm_inode_init_security()`（`evm_main.c:1064`）：新建 inode 时，若本次初始化会写入受保护 xattr，则用 `evm_init_hmac()` 计算 HMAC，并通过 `lsm_get_xattr_slot()` 在 `xattrs[]` 里追加一个名为 `XATTR_EVM_SUFFIX`（即 `security.evm`）的槽位，由 VFS 一并写入。
- `evm_inode_setattr()` / `evm_inode_setxattr()` / `evm_inode_removexattr()` 是 LSM hook 实现；`evm_secfs.c` 提供密钥加载接口；`evm_posix_acl.c` 处理 POSIX ACL 相关 xattr。

`evm_protect_xattr()`（`evm_main.c:520`）的判定顺序值得展开：写 `security.evm` 本身要求 `CAP_SYS_ADMIN` 且文件系统支持 HMAC；写其它受保护 xattr 前先 `evm_verify_current_integrity()` 校验现状，`NOXATTRS`（新文件）有 tmpfs/sysfs 与 `EVM_NEW_FILE` 豁免；`FAIL_IMMUTABLE` 的文件反而允许写其它 xattr（portable 签名永不更新，拦截无意义）；`PASS_IMMUTABLE` 则要求本次写入不改变受保护值（`evm_xattr_change()` 逐字节比较）。EVM 自己的 inode blob 是内嵌的 `struct evm_iint_cache`（`evm.h:40`，含 `evm_status:4` 与 `metadata_inode` 属性），与 IMA 的指针式 blob 不同，`evm_blob_sizes.lbs_xattr_count = 1` 为 `security.evm` 预留一个 xattr 槽位。

### 7.5 HMAC 输入构成与密钥来源

`evm_calc_hmac_or_hash()`（`evm_crypto.c:227`）是 HMAC 与签名摘要的共用实现：

1. 不支持 xattr 或 `s_user_ns != init_user_ns` 的文件系统直接返回 `-EOPNOTSUPP`（fuse 等用户命名空间挂载不参与 EVM）；
2. 遍历 `evm_config_xattrnames`，把每个受保护 xattr 的值按列表顺序喂入哈希；若调用者正在更新某个 xattr，直接用传入的**新值**而不是磁盘上的旧值——这就是“先算后写”的实现方式；
3. 本地计算的 HMAC/签名跳过 `enabled=false` 的 xattr；portable 签名则包含全部项，且**必须**包含 IMA 哈希（否则 `-EPERM`）；
4. 最后 `hmac_add_misc()` 混入 inode 元数据。

`hmac_add_misc()`（`evm_crypto.c:143`）的输入结构决定了 HMAC 的“绑定范围”：

```c
struct h_misc {
	unsigned long ino;      /* inode 号 */
	__u32 generation;       /* inode generation */
	uid_t uid;
	gid_t gid;
	umode_t mode;
} hmac_misc;
```

- portable 签名不含 `ino`/`generation`（否则无法跨文件系统迁移），也不含 fs UUID；
- uid/gid 按 **init user namespace** 编码而非文件系统自己的 user namespace——源码注释解释了防范的攻击：先在非特权 fuse 挂载里写好 xattr，再诱使系统下次启动正式挂载该文件系统并因“一切都签了名”而信任它；
- `EVM_ATTR_FSUUID` 启用时额外混入 `s_uuid`，把 HMAC 绑定到具体文件系统实例，防止整盘克隆。

**密钥来源**：HMAC 密钥是 128 字节静态缓冲 `evmkey`（`evm_crypto.c:27`）。`evm_init_key()`（`evm_crypto.c:439`）通过 `request_key(&key_type_encrypted, "evm-key", ...)` 取 encrypted 密钥，用其解密明文调用 `evm_set_key()`，随后**立即清零**密钥 payload 中的明文。encrypted 密钥本身由 TPM/trusted key 保护（见 [04-crypto-keyring.md](04-crypto-keyring.md)），用户态始终看不到 HMAC 密钥。`init_desc()` 惰性分配 `hmac(sha1)` tfm 并 `crypto_shash_setkey()`；密钥未加载时返回 `-ENOKEY` 并只打印一次 `HMAC key is not set`。

**不可变保护**：`evm_update_evmxattr()` 先查 `evm_is_immutable()`——portable 签名一旦验证通过（`EVM_IMMUTABLE_DIGSIG`），任何重写 `security.evm` 的尝试都返回 `-EPERM`。

## 8. 数字签名与密钥环

`security/integrity/digsig.c`：

```c
static struct key *keyring[INTEGRITY_KEYRING_MAX];
static const char * const keyring_name[INTEGRITY_KEYRING_MAX] = {
#ifndef CONFIG_INTEGRITY_TRUSTED_KEYRING
	"_evm", "_ima",
#else
	".evm", ".ima",
#endif
	".platform", ".machine",
};
```

即：启用 `CONFIG_INTEGRITY_TRUSTED_KEYRING` 时用 `.evm`/`.ima`，否则用不可被用户态直接链接的 `_evm`/`_ima`；`.platform` 与 `.machine` 分别由平台固件与机器密钥填充。

- `integrity_init_keyring(id)` 在 IMA/EVM 初始化时被调用，按 `CONFIG_INTEGRITY_TRUSTED_KEYRING`/`CONFIG_INTEGRITY_PLATFORM_KEYRING` 决定密钥环来源；
- `integrity_digsig_verify(id, sig, siglen, digest, digestlen, algo)`：

```c
keyring = integrity_keyring_from_id(id);
switch (sig[1]) {
case 1: return digsig_verify(keyring, sig + 1, siglen - 1, digest, digestlen);
case 2: return asymmetric_verify(keyring, sig, siglen, digest, digestlen);
case 3: return asymmetric_verify_v3(keyring, sig, siglen, digest, digestlen, algo);
}
```

`sig[1]` 是签名格式版本：v1 为裸 RSA 签名（xattr 类型字节已剥离），v2 为“文件数据哈希 + 签名”，v3 使用 `struct ima_file_id` 区分文件/缓冲区场景。`digsig_asymmetric.c` 通过 `asymmetric_verify()` 调用 `crypto/` 的 `public_key_verify_signature()`。

密钥来源：`.platform` 通常来自固件（UEFI db / device tree `/etc/keys`），`.machine` 来自 `CONFIG_SYSTEM_TRUSTED_KEYS`，`.evm`/`.ima` 在 initramfs 阶段由用户态装载。没有对应密钥时验签返回 `-ENOKEY`，评估降级为 `INTEGRITY_UNKNOWN`。

### 8.1 验签实现（`digsig_asymmetric.c`）

`asymmetric_verify()`（`digsig_asymmetric.c:135`）按签名头里的 `hdr->keyid`（`integrity.h:97` 注释：IMA 密钥标识符，不绑定 X509/PGP 格式）在密钥环中查找非对称密钥（`request_asymmetric_key()`），再由 `asymmetric_verify_common()` 填写 `struct public_key_signature` 并调用 `verify_signature()`：

```c
pks.hash_algo = hash_algo_name[hdr->hash_algo];
pks.pkey_algo = pk->pkey_algo;
if (!strcmp(pk->pkey_algo, "rsa"))
	pks.encoding = "pkcs1";
else if (!strncmp(pk->pkey_algo, "ecdsa-", 6))
	pks.encoding = "x962";
else if (!strcmp(pk->pkey_algo, "ecrdsa"))
	pks.encoding = "raw";
...
ret = verify_signature(key, &pks);
```

即 RSA 用 PKCS#1 v1.5 编码、ECDSA 用 X9.62、GOST ecrdsa 用 raw 编码；真正的密码学运算委托给 crypto API（见 [04-crypto-keyring.md](04-crypto-keyring.md)）。

**sigv3 与后量子签名**。v3 不再直接对文件摘要签名，而是对 `struct ima_file_id`（类型 + 算法 + 文件摘要）的摘要签名（`calc_file_id_hash()`，`digsig_asymmetric.c:175`），消除“文件数据签名”与“缓冲区签名”之间的歧义。对 ML-DSA（`pkey_algo` 以 `mldsa` 开头），`asymmetric_verify_v3()` 走 `asymmetric_verify_v3_hashless()`：`pks.hash_algo = "none"`，以 pure 模式直接对 `ima_file_id` 结构验签——这是本树中已经存在的后量子签名路径。

### 8.2 密钥环创建与链接限制

`integrity_init_keyring()`（`digsig.c:127`）不是简单的 `keyring_alloc`：

- `.platform` 与 `.machine`（未启用 `CONFIG_INTEGRITY_CA_MACHINE_KEYRING` 时）不设链接限制；
- 启用 `CONFIG_INTEGRITY_TRUSTED_KEYRING` 时，`.ima` 使用 `restrict_link_by_digsig_builtin`（启用 `CONFIG_IMA_KEYRINGS_PERMIT_SIGNED_BY_BUILTIN_OR_SECONDARY` 时为 `_and_secondary`）——新密钥必须由内置可信密钥签名；`.machine` 使用 `restrict_link_by_ca`，只允许 CA 证书；
- 除 `.machine` 外的密钥环带 `KEY_USR_WRITE`，允许用户态添加密钥；`.machine` 只能在启动期由 UEFI MOK 变量填充；
- `.platform` 创建后调用 `set_platform_trusted_keys()` 标记为平台信任锚；`.ima` 创建后 `load_module_cert()` 载入模块签名证书。

`integrity_load_x509()` 用 `kernel_read_file_from_path(..., READING_X509_CERTIFICATE)` 读证书文件，再 `key_create_or_update(..., "asymmetric", ...)` 加入指定密钥环——这条路径本身也受 IMA 的 `kernel_read_file` hook 度量，形成“完整性子系统度量自身输入”的自洽闭环。

## 9. 启动与调用链

```
start_kernel
  └─ security_init()                     （CONFIG_SECURITY 才存在）
       └─ ima/evm 作为 LSM 注册 hook（order = LSM_ORDER_LAST）
  └─ late_initcall(init_ima)             ima_main.c
       ├─ ima_iintcache_init()           创建 iint slab
       ├─ security_add_hooks(ima_hooks)
       └─ init_ima_appraise_lsm()
  └─ late_initcall(init_evm)             evm_main.c:1166
       ├─ evm_init_config()              受保护 xattr 列表
       ├─ evm_set_fixmode()              evm=fix
       ├─ integrity_init_keyring(INTEGRITY_KEYRING_EVM)
       └─ evm_init_secfs()
```

（EVM 的 LSM 注册走 `DEFINE_LSM(evm)` 的 `.init = init_evm_lsm`，`initcall_late = init_evm`。）

打开/执行文件时的调用链（以执行二进制为例）：

```
execve → bprm_check_security()
  → ima_bprm_check(bprm)
    → process_measurement(file, ..., BPRM_CHECK)
      → ima_get_action → ima_match_policy
      → measure: ima_collect_measurement → ima_store_measurement
      → appraise: ima_appraise_measurement → xattr_verify → evm_verifyxattr
      → audit: ima_audit_measurement
```

`mmap(PROT_EXEC)` 走 `ima_file_mmap()`，`open+read` 走 `ima_file_check()`，模块加载走 `ima_read_file()`/`ima_post_read_file()`，`kexec` 走 `ima_kexec.c` 的 `ima_kexec_cmdline`/`ima_add_kexec_buffer`。

## 10. 当前配置下的状态

- `CONFIG_SECURITY=n` 且 `CONFIG_INTEGRITY` 未启用：`security/integrity/` 不编译，`ima`/`evm` 不在 `lsm_idlist` 中，`securityfs` 下没有 `/sys/kernel/security/ima` 与 `/sys/kernel/security/evm`。
- `CONFIG_CRYPTO`、`CONFIG_KEYS` 未启用：即使单独打开 IMA/EVM 也无法满足 `select CRYPTO_HMAC`/`select KEYS`，Kconfig 会强制拉起或构建失败；因此本配置下不存在哈希/签名验签能力。
- 由于没有 IMA/EVM，`security.ima`、`security.evm` xattr 不会被内核读取或写入；用户态设置这些 xattr 只会作为普通 security xattr 存放（仍受 `CAP_SYS_ADMIN` 等限制），不具备完整性语义。
- 启用顺序上注意：IMA/EVM 是 `LSM_ORDER_LAST`，不受 `CONFIG_LSM` 列表约束；但 EVM 的初始化依赖密钥环，实际部署通常在 initramfs 中用 `evm_utils`/`ima-evm-utils` 装载 `.evm`/`.ima` 密钥后再开启策略。

## 11. 模块附加签名（modsig）与黑名单

内核模块可以在文件末尾携带 PKCS#7 附加签名（modsig），IMA 无需 `security.ima` xattr 即可评估它——这对不支持 xattr 的文件系统尤其有用。实现在 `ima_modsig.c`：

```
模块文件布局：
[ 模块本体 ][ PKCS#7 签名 ][ struct module_signature ][ "~Module signature appended~\n" ]
```

- `ima_read_modsig()`（`ima_modsig.c:40`）先比对缓冲区末尾的魔数（`MODULE_SIGNATURE_MARKER`，`include/uapi/linux/module_signature.h:15`），再 `mod_check_sig()` 校验头部，`pkcs7_parse_message()` 解析签名；
- `ima_collect_modsig()` 用 `pkcs7_supply_detached_data()` 把**去掉附加签名**的模块本体喂给 PKCS#7 代码，`pkcs7_get_digest()` 算出验签用摘要——注意它与 IMA 对整个文件（含签名字节）算的哈希不同；
- `ima_modsig_verify()` 调 `verify_pkcs7_message_sig(..., VERIFYING_MODULE_SIGNATURE, ...)`，复用模块签名验证路径与密钥环限制；
- 模板 `ima-modsig`（`d-ng|n-ng|sig|d-modsig|modsig`，`ima_template.c:26`）同时记录两种摘要与原始 PKCS#7；`process_measurement()` 对已度量文件在模板含 modsig 字段时会强制追加一条测量条目。

黑名单检查 `ima_check_blacklist()`（`ima_appraise.c:428`）在评估之前执行：对带 `appraise_flag=check_blacklist` 的规则，用 modsig 摘要或文件摘要查 `is_binary_blacklisted()`（`certs/blacklist.c:247`，黑名单来自 `.blacklist` 密钥环与 UEFI dbx）；命中返回 `-EPERM`，并按策略把 `blacklisted-hash` 作为缓冲区度量入日志。

## 12. kexec 迁移、早期密钥与日志管理

### 12.1 测量日志跨 kexec 携带

`CONFIG_IMA_KEXEC` 启用测量日志迁移（`ima_kexec.c`）：

```
kexec 加载路径：
  ima_add_kexec_buffer(image)          ima_kexec.c:156
    ├─ 跳过 KEXEC_TYPE_CRASH（crash kernel 不携带）
    ├─ 按 binary_runtime_size + 预留内存计算段大小
    └─ kexec_add_buffer()              把日志挂进 kexec 镜像

kexec 执行时：
  ima_update_kexec_buffer()            reboot notifier，优先级 INT_MIN
    └─ ima_dump_measurement_list()     重新序列化并 memcpy 进已映射的段

新内核启动：
  ima_load_kexec_buffer()              ima_kexec.c:292
    └─ ima_restore_measurement_list()  逐条恢复（不 extend TPM）
```

序列化格式以 `struct ima_kexec_hdr`（版本/条数/总大小）开头，后接每条的 pcr、SHA1 模板摘要、模板名与字段数据。`binary_runtime_size[]` 在追加时增量维护序列化尺寸，避免 kexec 时全量遍历；段大小超过 `totalram_pages() / 2` 会拒绝携带。恢复的条目经 `ima_restore_measurement_entry()` 加入日志但**不**进 htable、**不** extend TPM——新内核的 PCR 从固件度量重新开始，验证者靠 `kexec_load`/`kexec_execute` 事件区分新旧日志。`ima_kexec_post_load()` 用 `kimage_map_segment()` 映射源页，使执行阶段还能更新缓冲。

### 12.2 早期启动密钥的延后度量

`CONFIG_IMA_QUEUE_EARLY_BOOT_KEYS` 解决“密钥加载早于 IMA 策略就绪”的时序问题：`ima_queue_key()`（`ima_queue_keys.c:104`）把 `key_post_create_or_update` hook 捕获的密钥 payload 暂存队列；`ima_init_key_queue()` 启动一个 delayed work（`ima_keys_handler`），超时仍未处理则直接释放；策略就绪后 `ima_process_queued_keys()` 逐条度量并取消定时器。这保证 `.ima`/`.evm` 等早期密钥不会漏测。

### 12.3 securityfs 接口细节

`ima_fs_init()`（`ima_fs.c:773`）按 **TPM bank 算法**各创建一对读文件：`ascii_runtime_measurements_<algo>` 与 `binary_runtime_measurements_<algo>`（未知算法命名为 `tpm_alg_<alg_id>`），并提供无后缀兼容符号链接指向 SHA1 版本；staged 列表有独立的 `_staged` 文件与符号链接。`runtime_measurements_count` 与 `violations` 输出原子计数器。ascii 格式为 `pcr 模板摘要 模板名 字段数据...`，其中模板摘要是对应 bank 的摘要，验证者据此重放 PCR extend 序列并与 TPM quote 比对。

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `security/integrity/integrity.h` | `evm_ima_xattr_type`、`evm_ima_xattr_data`、`integrity_status`（实际在 `include/linux/integrity.h`）、xattr 结构 |
| `security/integrity/iint.c` | `integrity_inode_get/free` 通用 iint 缓存 |
| `security/integrity/ima/ima_iint.c` | iint slab 分配/释放、`S_IMA` 标志、overlayfs lockdep 标注 |
| `security/integrity/digsig.c` | `integrity_init_keyring()`、`integrity_digsig_verify()`、完整性密钥环定义 |
| `security/integrity/digsig_asymmetric.c` | `digsig_verify()`、`asymmetric_verify()`、`asymmetric_verify_v3()` |
| `security/integrity/integrity_audit.c` | `integrity_audit_msg()` 统一审计格式 |
| `security/integrity/ima/ima_main.c` | `process_measurement()`、各 `ima_*_check` hook、`DEFINE_LSM(ima)` |
| `security/integrity/ima/ima_policy.c` | `ima_rule_entry`、`ima_match_policy()`、内置策略数组、`ima_policy=` 解析 |
| `security/integrity/ima/ima_appraise.c` | `ima_appraise_measurement()`、`xattr_verify()`、`ima_appraise` 模式 |
| `security/integrity/ima/ima_api.c` | `ima_get_action()`、`ima_collect_measurement()`、`ima_store_measurement()`、`ima_audit_measurement()` |
| `security/integrity/ima/ima_crypto.c` | 文件/缓冲区哈希、`ima_calc_boot_aggregate()`、TPM extend |
| `security/integrity/ima/ima_queue.c` | `ima_htable`、`ima_add_template_entry()`、测量日志链表 |
| `security/integrity/ima/ima_template.c` / `ima_template_lib.c` | 模板描述与字段（`d-ng`/`n-ng`/`sig`/`buf`） |
| `security/integrity/ima/ima_fs.c` | `securityfs` 下的测量日志与 `policy` 接口 |
| `security/integrity/ima/ima_init.c` | `init_ima()` 启动顺序、boot aggregate |
| `security/integrity/ima/ima_kexec.c` | kexec 测量缓冲迁移 |
| `security/integrity/ima/ima_modsig.c` | 内核模块附加签名（modsig）解析 |
| `security/integrity/ima/ima_queue_keys.c` | 早期启动密钥延后度量 |
| `security/integrity/ima/Kconfig` | IMA 全部配置项 |
| `security/integrity/evm/evm_main.c` | `evm_verify_hmac()`、`evm_protect_xattr()`、`evm_inode_init_security()`、受保护 xattr 列表 |
| `security/integrity/evm/evm_crypto.c` | `evm_calc_hmac()`、`evm_calc_hash()`、`evm_init_hmac()`、`evm_update_evmxattr()` |
| `security/integrity/evm/evm_secfs.c` | `/sys/kernel/security/evm` 与 `evm_xattrs` |
| `security/integrity/evm/evm.h` | EVM 内部结构与 `evm_iint_cache` |
| `security/integrity/evm/Kconfig` | EVM 配置项 |
| `include/uapi/linux/xattr.h` | `XATTR_NAME_EVM`/`XATTR_NAME_IMA`/`XATTR_EVM_SUFFIX` |
| `include/linux/integrity.h` | `enum integrity_status` |
