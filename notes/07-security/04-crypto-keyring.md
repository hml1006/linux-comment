# 内核 Crypto API 与密钥环（Keyring）

> 源码基线 Linux 7.2。Crypto 部分位于 `crypto/` 与 `include/crypto/`，密钥管理位于 `security/keys/` 与 `include/linux/key*.h`。
>
> **配置前提**：当前 `.config` 中 `CONFIG_CRYPTO` 与 `CONFIG_KEYS` 都未启用。`CONFIG_CRYPTO` 关闭意味着 `crypto/` 下的算法、模板、AF_ALG（依赖 `CONFIG_CRYPTO_USER_API*`）都不编译，`/proc/crypto` 不存在；`CONFIG_KEYS` 关闭意味着 `security/keys/` 不编译，`add_key(2)`/`request_key(2)`/`keyctl(2)` 返回 `-ENOSYS`，`keyctl` 相关内核设施（含 `CONFIG_ENCRYPTED_KEYS`、`CONFIG_TRUSTED_KEYS`）全部不存在。这不是 7.2 缺少这些能力，而是本地调试配置做了裁剪。下文描述源码中完整存在的实现。

## 1. 总体分层

```
     用户态                   内核内调用者（文件系统、网络、IMA、dm-crypt…）
        │                                   │
        ├── PF_ALG socket（AF_ALG）          ├── crypto_alloc_skcipher()/crypto_alloc_aead()/...
        │        │                          │
        ▼        ▼                          ▼
   crypto/af_alg.c            crypto/api.c   算法查找 + tfm 创建
   crypto/algif_*.c           crypto/algapi.c 算法/模板/实例注册
        │                          │
        └────────────┬─────────────┘
                     ▼
              crypto/cbc.c、ecb.c、hmac.c、gcm.c …（模板，构造算法实例）
                     ▼
        具体驱动：crypto/aes_generic.c、arch/arm64/crypto/*、drivers/crypto/*
```

核心概念四层：

| 概念 | 结构 | 说明 |
|------|------|------|
| 算法（algorithm） | `struct crypto_alg` | 一个具体实现，如 `cbc(aes)` 实例或 `aes` 驱动 |
| 前端类型（type） | `struct crypto_type` | skcipher/aead/ahash/shash/akcipher/kpp，决定 ops 表与 ctx 布局 |
| 变换对象（tfm） | `struct crypto_tfm` 及其包装 | 带密钥的运行期句柄，由 `crypto_alloc_*` 创建 |
| 模板（template） | `struct crypto_template` | 以已有算法为参数生成新算法，如 `cbc`、`hmac` |

## 2. Crypto API 数据结构

### 2.1 `struct crypto_alg`（`include/linux/crypto.h:332`）

```c
struct crypto_alg {
	struct list_head cra_list;
	struct list_head cra_users;
	u32 cra_flags;
	unsigned int cra_blocksize;
	unsigned int cra_ctxsize;
	unsigned int cra_alignmask;
	unsigned int cra_reqsize;
	int cra_priority;
	refcount_t cra_refcnt;
	char cra_name[CRYPTO_MAX_ALG_NAME];        /* 通用名，如 "cbc(aes)" */
	char cra_driver_name[CRYPTO_MAX_ALG_NAME]; /* 驱动名，如 "cbc-aes-aesni" */
	const struct crypto_type *cra_type;
	union { struct cipher_alg cipher; } cra_u;
	int (*cra_init)(struct crypto_tfm *tfm);
	void (*cra_exit)(struct crypto_tfm *tfm);
	void (*cra_destroy)(struct crypto_alg *alg);
	struct module *cra_module;
	...
};
```

`cra_priority` 参与同名算法选择：查找时取优先级最高者。`cra_flags` 含 `CRYPTO_ALG_TYPE_*`、`CRYPTO_ALG_ASYNC`、`CRYPTO_ALG_NEED_FALLBACK`、`CRYPTO_ALG_INTERNAL` 等。

### 2.2 `struct crypto_tfm`（`include/linux/crypto.h:411`）

```c
struct crypto_tfm {
	u32 crt_flags;
	int node;
	struct crypto_tfm *fb;          /* fallback tfm */
	void (*exit)(struct crypto_tfm *tfm);
	struct crypto_alg *__crt_alg;
	void *__crt_ctx[] CRYPTO_MINALIGN_ATTR;  /* 算法私有 ctx，变长 */
};
```

所有前端 tfm 都是 `crypto_tfm` 的容器：

```c
struct crypto_skcipher { unsigned int reqsize; struct crypto_tfm base; };
struct crypto_lskcipher { struct crypto_tfm base; };
struct crypto_aead     { unsigned int authsize, reqsize; struct crypto_tfm base; };
struct crypto_ahash    { bool using_shash; unsigned int statesize, reqsize; struct crypto_tfm base; };
struct crypto_shash    { struct crypto_tfm base; };
```

`crypto_skcipher_ctx(tfm)` 等 inline 返回 `base.__crt_ctx`，模块实现通过它存放自己的上下文。

### 2.3 各前端算法 ops

| 类型 | 算法结构 | 主要回调 | 请求结构 |
|------|----------|----------|----------|
| skcipher（异步块加密） | `struct skcipher_alg` | `setkey/encrypt/decrypt/init/exit` | `struct skcipher_request` |
| lskcipher（线性/同步） | `struct lskcipher_alg`（`include/crypto/skcipher.h:203`） | `setkey/encrypt/decrypt`，直接 `(tfm, src, dst, len, siv, flags)` | 无请求结构 |
| aead（带关联数据的认证加密） | `struct aead_alg`（`include/crypto/aead.h:139`） | `setkey/setauthsize/encrypt/decrypt` | `struct aead_request` |
| ahash（异步哈希） | `struct ahash_alg`（`include/crypto/hash.h:153`） | `init/update/final/finup/digest/export/import/setkey` | `struct ahash_request` |
| shash（同步哈希） | `struct shash_alg`（`include/crypto/hash.h:246`） | 同上 + `descsize` | `struct shash_desc` |
| akcipher（非对称） | `struct akcipher_alg`（`include/crypto/akcipher.h:84`） | `encrypt/decrypt/set_pub_key/set_priv_key/max_size` | `struct akcipher_request` |
| kpp（密钥协商） | `struct kpp_alg`（`include/crypto/kpp.h:77`） | `set_secret/generate_public_key/compute_shared_secret/max_size` | `struct kpp_request` |

7.x 中 `cbc`/`ecb` 这类纯同步模式优先用 **lskcipher**（`crypto/lskcipher.c`，587 行），避免为每个块构造 `skcipher_request` 的开销；`crypto/skcipher.c`（886 行）仍提供异步接口并负责 lskcipher→skcipher 的适配。

## 3. 算法注册与查找

### 3.1 注册

`crypto_register_alg(struct crypto_alg *alg)`（`crypto/algapi.c:431`）：

1. 调用 `__crypto_register_alg()`（`algapi.c:302`）把算法挂到全局 `crypto_alg_list`（按 `cra_name`/`cra_driver_name` 排序），并查找是否有同名 `larval`（正在测试的占位算法）；
2. 若有 larval，则把算法交给它并唤醒等待者；否则 `crypto_alg_tested()` 路径之外直接可用；
3. 批量版本 `crypto_register_algs()`。

注销 `crypto_unregister_alg()` 会先标记 `CRYPTO_ALG_DEAD`，等待 `cra_refcnt` 归零后真正摘除。

### 3.2 查找（`crypto/api.c`）

```
crypto_alloc_skcipher("cbc(aes)", type, mask)
  └─ crypto_find_alg()                       api.c:535
       └─ crypto_alg_mod_lookup()            api.c:338
            ├─ crypto_larval_lookup()        api.c:290
            │    ├─ crypto_alg_lookup()      持 crypto_alg_sem 查表
            │    │    命中但 !TESTED → -ELIBBAD
            │    ├─ 未命中 → request_module("crypto-%s") / ("crypto-%s-all")
            │    └─ 找到 larval → crypto_larval_wait()
            └─ 权重/优先级选择
  └─ crypto_create_tfm_node(alg, frontend, node)  api.c:495
       ├─ crypto_check_alg()
       ├─ 分配 sizeof(crypto_tfm) + ctxsize + alignmask
       ├─ frontend->init() 初始化 ops
       └─ alg->cra_init()
```

关键点：

- **按需加载模块**：名字查找失败会 `request_module("crypto-cbc")`、`request_module("crypto-cbc-all")`；
- **自测（FIPS）**：`CONFIG_CRYPTO_MANAGER` 的 cryptomgr 会在算法首次使用时运行 `crypto/testmgr.c` 的 Known Answer Test，通过后置 `CRYPTO_ALG_TESTED`；未通过则返回 `-ELIBBAD`；
- **优先级与 fallback**：同名算法取 `cra_priority` 最高者；`CRYPTO_ALG_NEED_FALLBACK` 的算法若不可用，`crypto_create_tfm` 会分配 `tfm->fb` 指向软件实现；
- `/proc/crypto`（`crypto/proc.c`，`proc_create_seq("crypto", ...)`）列出 name/driver/module/priority/type/blocksize/ivsize 等信息，是排查算法可用性的第一入口。

### 3.3 典型使用

```c
struct crypto_skcipher *tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
if (IS_ERR(tfm))
	return PTR_ERR(tfm);
crypto_skcipher_setkey(tfm, key, keylen);

struct skcipher_request *req = skcipher_request_alloc(tfm, GFP_KERNEL);
skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP, done, NULL);
skcipher_request_set_crypt(req, src, dst, len, iv);
crypto_skcipher_encrypt(req);
```

`crypto_alloc_skcipher` 是 `crypto_alloc_tfm_node(name, &crypto_skcipher_type, node)` 的宏包装；`node` 用于 NUMA 绑定。

## 4. 模板与实例

### 4.1 结构（`include/crypto/algapi.h`）

```c
struct crypto_instance {
	struct crypto_alg alg;
	struct crypto_template *tmpl;
	union {
		struct hlist_node list;   /* 注册后：模板实例链表节点 */
		struct crypto_spawn *spawns; /* 注册前：spawn 链 */
	};
	void *__ctx[] CRYPTO_MINALIGN_ATTR;
};

struct crypto_template {
	struct list_head list;
	struct hlist_head instances;
	struct hlist_head dead;
	struct module *module;
	struct work_struct free_work;
	int (*create)(struct crypto_template *tmpl, struct rtattr **tb);
	char name[CRYPTO_MAX_ALG_NAME];
};

struct crypto_spawn {
	struct list_head list;
	struct crypto_alg *alg;     /* 被包裹的下层算法 */
	struct crypto_instance *inst;
	const struct crypto_type *frontend;
	u32 mask;
	bool dead, registered;
};
```

模板的 `create()` 收到解析好的属性表 `tb`（来自算法名 `cbc(aes)` 的内嵌参数），通过 `crypto_grab_*()` 获取下层算法，填写 `crypto_instance.alg` 后 `crypto_register_instance()` 注册为新算法。

### 4.2 示例：`cbc`

`crypto/cbc.c:157`：

```c
static struct crypto_template crypto_cbc_tmpl = {
	.name = "cbc",
	.create = crypto_cbc_create,
	.module = THIS_MODULE,
};
```

`crypto_cbc_create()` 用 `lskcipher_alloc_instance_simple(tmpl, tb)` 分配实例、`crypto_grab_lskcipher()` 抓取 `aes`，并包装 `setkey`/`encrypt`/`decrypt`。模块初始化 `crypto_cbc_module_init()` 只做 `crypto_register_template()`。

### 4.3 示例：`hmac`

`crypto/hmac.c` 同时提供 shash 与 ahash 两个模板入口：

```c
static struct crypto_template crypto_hmac_tmpl = {
	.name = "hmac",
	.create = hmac_create,          /* 内部区分 hmac_create_shash / hmac_create_ahash */
	.module = THIS_MODULE,
};
```

`__hmac_create_shash()` 通过 `crypto_grab_shash(spawn, shash_crypto_instance(inst), ...)` 包裹底层哈希（如 `sha256`），实现 `hmac(sha256)`；`hmac_import_state()` 等负责中间状态。

### 4.4 实例注册

`crypto_register_instance(tmpl, inst)`（`algapi.c:643`）会：

1. 对每个 `spawn` 调用 `crypto_init_spawn()` 绑定算法；
2. 把 `inst->alg` 加入到自己的名字哈希；
3. 失败时走 `crypto_remove_instance()`/`free_work` 异步释放。

## 5. AF_ALG：用户态 crypto 接口

### 5.1 编程模型

AF_ALG 把 crypto 算法暴露成 socket：

```c
int fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
struct sockaddr_alg sa = {
	.salg_family = AF_ALG,
	.salg_type = "skcipher",
	.salg_name = "cbc(aes)",
};
bind(fd, (struct sockaddr *)&sa, sizeof(sa));
int op = accept(fd, NULL, 0);
setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, keylen);
sendmsg(op, ...);   /* cmsg: ALG_SET_IV / ALG_SET_OP */
read(op, ...);      /* 加密/解密结果 */
```

`include/uapi/linux/if_alg.h` 定义：

- `struct sockaddr_alg { __u16 salg_family; __u8 salg_type[14]; __u32 salg_feat; __u32 salg_mask; __u8 salg_name[64]; }`，以及 `sockaddr_alg_new` 变长版本；
- `ALG_SET_KEY=1`、`ALG_SET_IV=2`、`ALG_SET_OP=3`、`ALG_SET_AEAD_ASSOCLEN=4`、`ALG_SET_AEAD_AUTHSIZE=5`、`ALG_SET_DRBG_ENTROPY=6`、`ALG_SET_KEY_BY_KEY_SERIAL=7`。

### 5.2 内核实现（`crypto/af_alg.c`，1252 行）

- `alg_create()`：`sock_alloc` + `sk_alloc(..., PF_ALG, ..., &alg_proto, ...)`，`sock->ops = &alg_proto_ops`；
- `alg_bind()`（`af_alg.c:148`）：按 `salg_type` 用 `alg_get_type()` 在 `alg_types` 链表里找到已注册的 `af_alg_type`，再 `type->bind()` 用 `salg_name` 调用 `crypto_alloc_*` 得到 tfm；
- `alg_setsockopt()`（`af_alg.c:355`）：处理 `ALG_SET_KEY`（`type->setkey`）、`ALG_SET_KEY_BY_KEY_SERIAL`（从 keyring 取密钥，`alg_setkey_by_key_serial()`）、`ALG_SET_AEAD_AUTHSIZE`、`ALG_SET_DRBG_ENTROPY`；已 `connect` 后返回 `-EBUSY`；
- `af_alg_accept()`（`af_alg.c:470`）：为每次 `accept` 创建子 socket，`sk2 = sk_alloc(...)`，`type->accept` 复制/引用 tfm；`refcnt`/`nokey_refcnt` 控制“设置密钥后是否禁用无密钥操作”；
- `af_alg_sendmsg()`/`af_alg_sendpage()`/`af_alg_wait_for_data()`：统一的 `sendmsg`/`read` 辅助，解析 cmsg 中的 `ALG_SET_IV`/`ALG_SET_OP`。

### 5.3 类型实现

| 文件 | 类型名 | 说明 |
|------|--------|------|
| `crypto/algif_skcipher.c` | `"skcipher"` | 对称块加密，`algif_type_skcipher`，支持 `ALG_SET_IV/OP` |
| `crypto/algif_aead.c` | `"aead"` | 认证加密，额外支持 `ALG_SET_AEAD_AUTHSIZE/ASSOCLEN` |
| `crypto/algif_hash.c` | `"hash"` | 哈希与 HMAC，`hash_setkey` 支持带密钥摘要 |
| `crypto/algif_rng.c` | `"rng"` | 随机数，`ALG_SET_DRBG_ENTROPY` |

每个文件在 `module_init` 中 `af_alg_register_type()`，注册到 `af_alg.c` 的全局 `alg_types` 链表（`DECLARE_RWSEM(alg_types_sem)` 保护）。AF_ALG 需要 `CONFIG_CRYPTO_USER_API`/`CONFIG_CRYPTO_USER_API_SKCIPHER` 等，本配置下均不存在。

## 6. 密钥环（Keyring）

### 6.1 `struct key`（`include/linux/key.h:195`）

```c
struct key {
	refcount_t usage;
	key_serial_t serial;          /* 用户态可见的序列号 */
	union { struct list_head graveyard_link; struct rb_node serial_node; };
	struct rw_semaphore sem;
	struct key_user *user;
	void *security;               /* LSM blob */
	union { time64_t expiry; time64_t revoked_at; };
	time64_t last_used_at;
	kuid_t uid;
	kgid_t gid;
	key_perm_t perm;
	unsigned short quotalen;
	unsigned short datalen;
	short state;                  /* 正数=实例化，负数=构造失败原因 */
	unsigned long flags;          /* KEY_FLAG_DEAD/REVOKED/IN_QUOTA/... */
	union {
		struct keyring_index_key index_key;
		struct {
			unsigned long hash;
			unsigned long len_desc;
			struct key_type *type;
			struct key_tag *domain_tag;
			char *description;
		};
	};
	union {
		struct list_head link;      /* 普通 key */
		struct assoc_array keys;    /* keyring 的索引 */
		...
	};
	...
};
```

`struct keyring_index_key` 由 `(type, description, domain_tag)` 组成，`hash_key_type_and_desc()`（`keyring.c:163`）计算 `hash`，用作 keyring 内部 `assoc_array` 的索引。keyring 本体就是一棵 `struct assoc_array`，因此查找/插入是 O(log n) 且支持 RCU 并发读。

`struct key_type`（`include/linux/key-type.h:64`）定义一类 key 的行为：

```c
struct key_type {
	const char *name;
	size_t def_datalen;
	unsigned int flags;      /* KEY_TYPE_NET_DOMAIN / KEY_TYPE_INSTANT_REAP */
	int (*vet_description)(const char *description);
	int (*preparse)(struct key_preparsed_payload *prep);
	void (*free_preparse)(...);
	int (*instantiate)(struct key *key, struct key_preparsed_payload *prep);
	int (*update)(...);
	int (*match_preparse)(...);
	void (*revoke)(struct key *key);
	void (*destroy)(struct key *key);
	void (*describe)(const struct key *key, struct seq_file *m);
	long (*read)(const struct key *key, char *buffer, size_t buflen);
	...
};
```

### 6.2 已注册的 key 类型

| 类型名 | 结构 | 源文件 |
|--------|------|--------|
| `"keyring"` | `key_type_keyring` | `security/keys/keyring.c:84` |
| `"user"` | `key_type_user` | `security/keys/user_defined.c:23` |
| `"logon"` | `key_type_logon` | `security/keys/user_defined.c:43`（不可读） |
| `"big_key"` | `key_type_big_key` | `security/keys/big_key.c:42`（大负载，可能落 tmpfs） |
| `"encrypted"` | `key_type_encrypted` | `security/keys/encrypted-keys/encrypted.c:973` |
| `"trusted"` | `key_type_trusted` | `security/keys/trusted-keys/trusted_core.c:328`（TPM） |
| `"asymmetric"` | `key_type_asymmetric` | `crypto/asymmetric_keys/asymmetric_type.c:607` |
| `"request_key_auth"` | `key_type_request_key_auth` | `security/keys/request_key_auth.c:31` |
| `"dead"` | `key_type_dead` | `security/keys/gc.c:43`（已回收占位） |
| `"dns_resolver"` | `key_type_dns_resolver` | `net/dns_resolver/dns_key.c:310` |
| `"blacklist"` | `key_type_blacklist` | `certs/blacklist.c:139` |
| `"pkcs7"` | `key_type_pkcs7` | `crypto/asymmetric_keys/pkcs7_key_type.c:70` |

`key_type_lookup(type)`/`register_key_type()`/`unregister_key_type()` 维护全局类型表。

### 6.3 权限模型

权限位（`include/linux/key.h:40` 起）分四组，各 6 位：

```
KEY_POS_VIEW/READ/WRITE/SEARCH/LINK/SETATTR   (0x01000000 ... 0x20000000)
KEY_USR_VIEW/...                              (0x00010000 ... 0x00200000)
KEY_GRP_VIEW/...                              (0x00000100 ... 0x00002000)
KEY_OTH_VIEW/...                              (0x00000001 ... 0x00000020)
```

判定函数 `key_task_permission(key_ref, cred, need_perm)`（`security/keys/permission.c`）：

1. `need_perm` 为 `KEY_NEED_UNLINK`/`KEY_SYSADMIN_OVERRIDE`/`KEY_AUTHTOKEN_OVERRIDE`/`KEY_DEFER_PERM_CHECK` 时直接跳到 LSM 检查；
2. 否则按 `need_perm` 选 `KEY_OTH_*` 掩码；
3. 若 `key->uid == cred->fsuid`，改用 `key->perm >> 16`（`KEY_USR_*`）；
4. 否则若 gid 匹配，改用 `key->perm >> 8`（`KEY_GRP_*`）；
5. 否则使用 `KEY_OTH_*`（`kperm = key->perm`）；
6. possessor 权限是**叠加**的：`is_key_possessed(key_ref)` 为真时 `kperm |= key->perm >> 24`（即 `KEY_POS_*`），不替代其它组；
7. 最后调用 LSM hook `security_key_permission()` 允许模块（SELinux/Smack）覆盖判定。

`enum key_need_perm`（`include/linux/key.h:78`）比 POSIX 权限更细：`VIEW/READ/WRITE/SEARCH/LINK/SETATTR/UNLINK` 加三种特殊值。

### 6.4 关键操作与调用链

**创建/更新**：

```
add_key(2) → SYSCALL_DEFINE5(add_key)            security/keys/keyctl.c:74
  → key_create_or_update(keyring_ref, type, desc, payload, plen, perm, flags)
    → __key_create_or_update()                   security/keys/key.c:806
       ├─ key_type_lookup(type)
       ├─ keyring_search(keyring_ref, type, desc, ...) 查已有 key
       ├─ 命中 → key->type->update()（需 KEY_NEED_WRITE）
       └─ 未命中 → key_alloc() → key->type->instantiate() → key_link()
```

`key_alloc()`（`security/keys/key.c:192`）做配额检查（`key_user->qnkeys/qnbytes`）、分配序列号（`key_alloc_serial()`，用 `key_serial_rwsem` 与 `key_serial_tree` 红黑树防重）、初始化状态为 `KEY_IS_UNINSTANTIATED`。

**搜索**：`keyring_search(key_ref, type, description, recurse)`（`keyring.c:942`）→ `search_nested_keyrings()` 递归遍历 keyring 树，使用 `assoc_array_find()` 命中后检查 `KEY_NEED_SEARCH`，并跳过 revoked/expired/未实例化 key。

**request_key**：

```
request_key(2) → request_key_and_link()          security/keys/request_key.c:574
  ├─ 先在 thread/process/session 等 keyring 中搜索
  ├─ 未命中 → 构造 request_key_auth key，call_usermodehelper("/sbin/request-key", ...)
  ├─ 等待 key 状态变为实例化（或超时）
  └─ 成功后把 key 链接到目标 keyring
```

**进程 keyring**（`security/keys/process_keys.c`）：每个 `cred` 持有

- `thread_keyring`（线程级，`KEY_SPEC_THREAD_KEYRING = -1`）
- `process_keyring`（进程级，`-2`）
- `session_keyring`（会话级，`-3`）
- 另有 `user_keyring`（`-4`，按 UID）、`user_session_keyring`（`-5`）、`group_keyring`（`-6`）、`reqkey_auth_key`（`-7`）、`requestor_keyring`（`-8`），定义于 `include/uapi/linux/keyctl.h:19`。

`install_thread_keyring_to_cred()`/`install_process_keyring_to_cred()`/`install_session_keyring_to_cred()` 在 fork/exec/`setsid` 时复制或新建，避免不同进程共享可变密钥。

**特殊 keyring**：`.builtin_trusted`/`.secondary_trusted`（内核密钥）、`.platform`/`.machine`/`.evm`/`.ima`（完整性）、`.blacklist`（模块黑名单）、`.persistent`（用户持久）等，由 `CONFIG_*` 决定。

## 7. 当前配置下的状态

- `CONFIG_CRYPTO=n`：`crypto/` 不编译；`/proc/crypto` 不存在；`AF_ALG`（`CONFIG_CRYPTO_USER_API*`）不可用；任何依赖 crypto 的子系统（dm-crypt、IPsec、Wi-Fi 加密、IMA 哈希）都无法启用。
- `CONFIG_KEYS=n`：`security/keys/` 不编译；`add_key`/`request_key`/`keyctl` 系统调用不存在（调用返回 `-ENOSYS`）；`key->security` LSM blob 与 `security_key_*` hook 也随之消失。
- 由于 `CONFIG_KEYS` 关闭，`CONFIG_ENCRYPTED_KEYS`、`CONFIG_TRUSTED_KEYS`、`CONFIG_ASYMMETRIC_KEY_TYPE` 及其下游（模块签名验证 `CONFIG_MODULE_SIG` 的部分路径、`CONFIG_INTEGRITY` 验签）在本构建中不可用。
- 若需在本机构建启用，最小集合：`CONFIG_CRYPTO=y`、`CONFIG_CRYPTO_MANAGER=y`、`CONFIG_CRYPTO_HASH/SKCIPHER/AEAD/...` 以及具体算法；密钥侧 `CONFIG_KEYS=y`，按需加 `CONFIG_KEY_DH_OPERATIONS`、`CONFIG_BIG_KEYS`、`CONFIG_ENCRYPTED_KEYS`、`CONFIG_TRUSTED_KEYS`、`CONFIG_ASYMMETRIC_KEY_TYPE`。
- 需要从用户态使用 crypto 时还要打开 `CONFIG_CRYPTO_USER_API`、`CONFIG_CRYPTO_USER_API_HASH`、`CONFIG_CRYPTO_USER_API_SKCIPHER`、`CONFIG_CRYPTO_USER_API_AEAD`、`CONFIG_CRYPTO_USER_API_RNG`。

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `include/linux/crypto.h` | `crypto_alg`、`crypto_tfm`、`crypto_type`、`crypto_alloc_*` 原型 |
| `include/crypto/skcipher.h` | `crypto_skcipher`、`crypto_lskcipher`、`lskcipher_alg`、`skcipher_alg_common` |
| `include/crypto/aead.h` | `aead_alg`、`crypto_aead`、`aead_request` |
| `include/crypto/hash.h` | `ahash_alg`、`shash_alg`、`crypto_ahash/shash`、`shash_desc` |
| `include/crypto/akcipher.h` | `akcipher_alg`、`struct akcipher_request` |
| `include/crypto/kpp.h` | `kpp_alg`、`struct kpp_request` |
| `include/crypto/algapi.h` | `crypto_instance`、`crypto_template`、`crypto_spawn`、`crypto_grab_*` |
| `crypto/api.c` | `crypto_alg_mod_lookup()`、`crypto_larval_lookup()`、`crypto_create_tfm_node()`、`crypto_find_alg()` |
| `crypto/algapi.c` | `crypto_register_alg()`、`crypto_register_template()`、`crypto_register_instance()`、`crypto_grab_spawn()` |
| `crypto/skcipher.c` | skcipher 前端、请求处理、`CRYPTO_TFM_NEED_KEY` 检查 |
| `crypto/lskcipher.c` | 线性 skcipher 前端（`cbc`/`ecb` 的基础） |
| `crypto/aead.c` `ahash.c` `shash.c` `akcipher.c` `kpp.c` | 各前端 tfm 分配与请求封装 |
| `crypto/cbc.c` `ecb.c` `hmac.c` `gcm.c` 等 | 模板实现（`crypto_template` + `create`） |
| `crypto/proc.c` | `/proc/crypto` 算法清单 |
| `crypto/testmgr.c` | 算法自测（KAT），`CRYPTO_ALG_TESTED` |
| `crypto/af_alg.c` | AF_ALG 核心：`alg_bind`、`alg_setsockopt`、`af_alg_accept`、`af_alg_sendmsg` |
| `crypto/algif_skcipher.c` `algif_aead.c` `algif_hash.c` `algif_rng.c` | AF_ALG 具体类型 |
| `include/uapi/linux/if_alg.h` | `sockaddr_alg`、`ALG_SET_*` |
| `include/linux/key.h` | `struct key`、`keyring_index_key`、权限位、`enum key_need_perm` |
| `include/linux/key-type.h` | `struct key_type` 及其回调 |
| `include/uapi/linux/keyctl.h` | `KEY_SPEC_*`、`KEYCTL_*` |
| `security/keys/key.c` | `key_alloc()`、`key_create_or_update()`、配额与序列号 |
| `security/keys/keyring.c` | keyring 的 `assoc_array` 索引、`keyring_search()`、`keyring_alloc()` |
| `security/keys/permission.c` | `key_task_permission()` |
| `security/keys/process_keys.c` | thread/process/session keyring 安装与 `lookup_user_key()` |
| `security/keys/request_key.c` | `request_key_and_link()`、`/sbin/request-key` 上升调用 |
| `security/keys/user_defined.c` | `user`/`logon` key 类型 |
| `security/keys/big_key.c` `encrypted-keys/` `trusted-keys/` | 大负载、加密、TPM 可信 key |
| `security/keys/keyctl.c` | `add_key`/`request_key`/`keyctl` 系统调用入口 |
| `crypto/asymmetric_keys/asymmetric_type.c` | `asymmetric` key 类型（供验签使用） |
