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

### 4.3 测量日志与 TPM

`ima_store_measurement()` → `ima_alloc_init_template()`（按 `template_desc` 逐字段填充）→ `ima_add_template_entry()`（`ima_queue.c:242`）：

- 若存在 TPM（`ima_tpm_chip`），对模板数据做 `tpm_pcr_extend()` 扩展到对应 PCR；
- boot aggregate 由 `ima_calc_boot_aggregate()` 计算：对 PCR 0-7 做累积摘要，非 SHA1 bank 还会把 PCR 8-9 计入；
- `ima_add_digest_entry()` 用 `(inode, version)` 查 `ima_htable` 去重，然后挂到 `ima_measurements` 链表；
- 支持 staged 列表（`ima_measurements_staged`）以便 kexec 迁移时整体替换；
- 测量日志可通过 `securityfs` 读取：`/sys/kernel/security/ima/ascii_runtime_measurements`、`binary_runtime_measurements`、`runtime_measurements_count`、`violations`（`ima_fs.c:773` 的 `ima_fs_init()`）。

启动时 `init_ima()`（`ima_init.c`）依次：`integrity_init_keyring(INTEGRITY_KEYRING_IMA)` → `ima_init_crypto()` → `ima_init_template()` → `ima_init_digests()` → `ima_init_htable()` → `ima_add_boot_aggregate()`（boot aggregate 必须作为第一条记录，聚合 TPM 0-7 PCR 或使用 `boot_aggregate` 模板）→ `ima_init_policy()` → `ima_fs_init()`。无 TPM 时打印 `No TPM chip found, activating TPM-bypass!`，度量仍记录但不 extend。

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

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `security/integrity/integrity.h` | `evm_ima_xattr_type`、`evm_ima_xattr_data`、`integrity_status`（实际在 `include/linux/integrity.h`）、xattr 结构 |
| `security/integrity/iint.c` | `integrity_inode_get/free` 通用 iint 缓存 |
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
