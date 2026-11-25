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

前端类型本身由 `struct crypto_type` 描述（`crypto/internal.h:35`），它是 `crypto_create_tfm_node()` 的“前端插件”：

```c
struct crypto_type {
	unsigned int (*ctxsize)(struct crypto_alg *alg, u32 type, u32 mask);
	unsigned int (*extsize)(struct crypto_alg *alg);
	int (*init_tfm)(struct crypto_tfm *tfm);
	void (*show)(struct seq_file *m, struct crypto_alg *alg);
	int (*report)(struct sk_buff *skb, struct crypto_alg *alg);
	void (*free)(struct crypto_instance *inst);
	void (*destroy)(struct crypto_alg *alg);
	unsigned int type;
	unsigned int maskclear, maskset;
	unsigned int tfmsize, algsize;
};
```

`ctxsize` 决定 tfm 私有区大小，`init_tfm` 安装前端 ops（如 `crypto_skcipher_type` 的 `crypto_skcipher_init_tfm`），`show` 负责 `/proc/crypto` 中该类型的字段输出，`tfmsize/algsize` 用于实例分配。`crypto_skcipher_type`、`crypto_ahash_type` 等全局实例分别在各前端文件中定义。

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

适配点就在请求入口（`skcipher.c:435`）：

```c
int crypto_skcipher_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);

	if (crypto_skcipher_get_flags(tfm) & CRYPTO_TFM_NEED_KEY)
		return -ENOKEY;
	if (alg->co.base.cra_type != &crypto_skcipher_type)
		return crypto_lskcipher_encrypt_sg(req);
	return alg->encrypt(req);
}
```

未 setkey 的 tfm 带 `CRYPTO_TFM_NEED_KEY` 标志，任何加解密直接 `-ENOKEY`；lskcipher 后端的算法由 `crypto_lskcipher_encrypt_sg()` 把 scatterlist 请求转成线性调用。

异步 skcipher 驱动普遍用 **skcipher_walk** 抽象遍历 scatterlist：`skcipher_walk_virt()`（`skcipher.c:283`）初始化 `walk->total/iv/blocksize/stride`，`skcipher_walk_done()` 推进到下一段；按对齐与映射情况分 fast（直接映射）、copy（bounce buffer）、slow（临时分配）三条路径，`CRYPTO_TFM_REQ_MAY_SLEEP` 决定 `SKCIPHER_WALK_SLEEP`。`stride` 取自 `alg->walksize`，供 SIMD 驱动一次处理多块；AEAD 有对应的 `skcipher_walk_aead_encrypt/decrypt()` 变体。

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

### 3.4 larval：占位算法与自测流水线

“先测试后使用”的语义靠 **larval**（幼体）占位算法实现（`crypto/api.c`）：

```c
// api.c:104 crypto_larval_alloc()
larval->alg.cra_flags = CRYPTO_ALG_LARVAL | type;
larval->alg.cra_priority = -1;
larval->alg.cra_destroy = crypto_larval_destroy;
strscpy(larval->alg.cra_name, name, CRYPTO_MAX_ALG_NAME);
init_completion(&larval->completion);
```

完整流水线：

```
crypto_alg_mod_lookup()                    api.c:338
  ├─ crypto_larval_lookup()                api.c:290
  │    ├─ crypto_alg_lookup() 命中已 TESTED 算法 → 直接返回
  │    ├─ 未命中且 !CRYPTO_NOLOAD → request_module("crypto-<name>")
  │    └─ 仍未命中 → crypto_larval_add() 挂 LARVAL 占位到全局链表
  ├─ crypto_probing_notify(CRYPTO_MSG_ALG_REQUEST)
  │    └─ cryptomgr（algboss.c）调度 kthread cryptomgr_probe
  │         ├─ 构造模板实例（如 cbc(aes)）
  │         └─ alg_test() 自测 → 成功后置 CRYPTO_ALG_TESTED
  └─ crypto_larval_wait()                  api.c:201
       └─ wait_for_completion_killable_timeout(..., 60 * HZ)
```

要点：

- 等待者阻塞在 larval 的 `completion` 上，超时 **60 秒**；超时则 kill 测试 larval 并返回 `-ETIMEDOUT`；两个探测并发（名字与驱动名各一）时以 `-EEXIST` 重查化解；
- `crypto_alg_lookup()`（`api.c:253`）默认只接受带 `CRYPTO_ALG_TESTED` 的算法；名字命中但测试失败返回 `-ELIBBAD`；
- `CRYPTO_ALG_FIPS_INTERNAL` 标记 FIPS 模式内部专用算法，外部查找会被拒绝；`CRYPTO_ALG_INTERNAL` 则要求调用者显式带标志才能使用；
- 自测主体 `alg_test()`（`testmgr.c:5618`）按 `alg_test_desc[]` 跑 Known Answer Test；`CONFIG_CRYPTO_SELFTESTS` 控制开关，`CONFIG_CRYPTO_SELFTESTS_FULL` 追加 fuzz 测试（`fuzz_iterations` 默认 100）与慢速测试；模块参数 `notests`/`noslowtests` 可运行时关闭；
- 未启用 `CONFIG_CRYPTO_SELFTESTS` 时 `alg_test()` 是返回 0 的空函数（`testmgr.c:63`），算法注册后直接可用。

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

CBC 加解密本身是纯块循环（`crypto/cbc.c:15`），区分 inplace 与非 inplace 两条路径：

```c
// cbc.c:21 加密非 inplace 路径
for (; nbytes >= bsize; src += bsize, dst += bsize, nbytes -= bsize) {
	crypto_xor(iv, src, bsize);
	crypto_lskcipher_encrypt(tfm, iv, dst, bsize, NULL);
	memcpy(iv, dst, bsize);
}
```

inplace 解密（`crypto_cbc_decrypt_inplace()`）必须**从最后一个块倒序**处理，先保存末块作为 `last_iv`——因为每块的“IV”是前一块的密文，正序处理会被覆盖。`crypto_cbc_create()`（`cbc.c:138`）还要求底层块大小为 2 的幂且 `statesize == 0`（CBC 无中间状态）；剩余字节非零且带 `CRYPTO_LSKCIPHER_FLAG_FINAL` 时返回 `-EINVAL`。

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

spawn 是实例与底层算法之间的引用桥梁。`crypto_grab_spawn()`（`algapi.c:721`）把 spawn 挂到目标算法的 `cra_users` 链表，并继承标志：

```c
down_write(&crypto_alg_sem);
if (!crypto_is_moribund(alg)) {
	list_add(&spawn->list, &alg->cra_users);
	spawn->alg = alg;
	spawn->next = inst->spawns;
	inst->spawns = spawn;
	inst->alg.cra_flags |=
		(alg->cra_flags & CRYPTO_ALG_INHERITED_FLAGS);
	err = 0;
}
up_write(&crypto_alg_sem);
```

底层算法注销时，`crypto_remove_spawns()` 把所有依赖它的 spawn 标记为 dead 并 shoot 实例（`crypto_shoot_alg()`）；实例经 `crypto_spawn_tfm()`（`algapi.c:797`）分配底层 tfm 时若发现算法 moribund，会触发 shoot 并返回 `-EAGAIN`，由调用者回退重建。这就是“底层驱动卸载 → 上层模板实例失效”的传播链。

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

`ALG_SET_KEY_BY_KEY_SERIAL` 是 AF_ALG 与密钥环的桥梁（`alg_setkey_by_key_serial()`，`af_alg.c:286`）：用户态只传 4 字节 key 序列号，内核 `lookup_key()` 取 key 后按类型读 payload：

```c
if (!strcmp(key->type->name, "user") ||
    !strcmp(key->type->name, "logon"))
	ret = key_data_ptr_user(key, &key_datalen);
else if (IS_REACHABLE(CONFIG_ENCRYPTED_KEYS) &&
	   !strcmp(key->type->name, "encrypted"))
	ret = key_data_ptr_encrypted(key, &key_datalen);
else if (IS_REACHABLE(CONFIG_TRUSTED_KEYS) && ...)
```

这样密钥明文不必出现在 `setsockopt` 缓冲区里——对 `logon`/`encrypted`/`trusted` key，用户态根本读不到 payload，却仍能让内核拿它做密码运算。`CONFIG_KEYS` 关闭时该选项是返回 `-ENOPROTOOPT` 的 stub（`af_alg.c:346`）。

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

key 的生命周期状态存于 `key->state`：`KEY_IS_UNINSTANTIATED`（0，已分配未填 payload）、`KEY_IS_POSITIVE`（1，已实例化）、负值为拒绝错误码（negative key，由 `key_reject_and_link()` 写入）。实例化的提交点是 `mark_key_instantiated()`（`key.c:412`）：

```c
smp_store_release(&key->state,
		  (reject_error < 0) ? reject_error : KEY_IS_POSITIVE);
```

release 屏障与 `key_read_state()` 的 acquire 读配对，保证 payload 先于状态可见。`__key_instantiate_and_link()`（`key.c:427`）在 `key_construction_mutex` 下把“实例化 + 链接 + 作废 authkey + 唤醒等待者”串成原子操作；key 只能实例化一次，重复调用返回 `-EBUSY`。

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

`key_task_permission()`（`permission.c:26`）的核心判定可浓缩为：

```c
if (uid_eq(key->uid, cred->fsuid))
	kperm = key->perm >> 16;          /* owner */
else if (gid 匹配 fsgid 或补充组)
	kperm = key->perm >> 8;           /* group */
else
	kperm = key->perm;                /* other */

if (is_key_possessed(key_ref))
	kperm |= key->perm >> 24;         /* possessor 权限叠加 */

if ((kperm & mask) != mask)
	return -EACCES;
return security_key_permission(key_ref, cred, need_perm);
```

注意 gid 分支还要求 `key->perm & KEY_GRP_ALL` 非零，并用 `groups_search()` 检查补充组；LSM hook 是最终裁决者（hook 框架见 [01-lsm.md](01-lsm.md)）。

`key_validate()`（`permission.c:102`）负责状态校验，错误语义严格区分：

| 条件 | 返回值 |
|------|--------|
| `KEY_FLAG_INVALIDATED` | `-ENOKEY` |
| `KEY_FLAG_REVOKED` 或 `KEY_FLAG_DEAD` | `-EKEYREVOKED` |
| `expiry` 到期 | `-EKEYEXPIRED` |

搜索迭代器 `keyring_search_iterator()`（`keyring.c:576`）用同一组标志跳过无效 key，并把具体错误记入 `ctx->result`，供调用者区分“没找到”与“找到但不可用”。

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

`key_alloc()`（`security/keys/key.c:224`）做配额检查（`key_user->qnkeys/qnbytes`）、分配序列号（`key_alloc_serial()`，用 `key_serial_rwsem` 与 `key_serial_tree` 红黑树防重）、初始化状态为 `KEY_IS_UNINSTANTIATED`。

配额参数在 `key.c:28`：普通用户默认 `key_quota_maxkeys = 200`、`key_quota_maxbytes = 20000`，root 用更大的 `key_quota_root_maxkeys/maxbytes`（可经 `/proc/sys/kernel/keys/` 调整）。`key_alloc()` 在 `user->lock` 下检查 `qnkeys + 1 > maxkeys` 或 `qnbytes + quotalen > maxbytes`，超限返回 `-EDQUOT`；`KEY_ALLOC_NOT_IN_QUOTA`（内核内部 key 用）跳过配额，`KEY_ALLOC_QUOTA_OVERRUN` 允许临时超限。分配成功后先 `security_key_alloc()` 初始化 LSM blob，最后 `key_alloc_serial()` 把 key 发布进 `key_serial_tree` 红黑树——序列号是用户态唯一可见的句柄。

**搜索**：`keyring_search(key_ref, type, description, recurse)`（`keyring.c:942`）→ `search_nested_keyrings()` 递归遍历 keyring 树，使用 `assoc_array_find()` 命中后检查 `KEY_NEED_SEARCH`，并跳过 revoked/expired/未实例化 key。

`search_nested_keyrings()`（`keyring.c:665`）不用函数递归而用显式栈，深度上限 `KEYRING_SEARCH_MAX_DEPTH = 6`（`keyring.c:28`），防止恶意嵌套 keyring 耗尽内核栈：

```c
struct {
	struct key *keyring;
	struct assoc_array_node *node;
	int slot;
} stack[KEYRING_SEARCH_MAX_DEPTH];
```

搜索有两种模式：`KEYRING_SEARCH_LOOKUP_DIRECT` 用 `assoc_array_find()` 按 index key 直接定位（O(log n)）；`LOOKUP_ITERATE` 用自定义 match 函数遍历整棵树（`dns_resolver` 等类型用）。`assoc_array` 的 shortcut 节点压缩长公共前缀，搜索路径对 shortcut 与 node 分别处理。遍历中遇到 keyring 就压栈继续搜，possessed 标志沿搜索路径传播——这正是 6.3 节“possessor 权限”的来源。

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

`lookup_user_key()`（`process_keys.c:611`）是所有 keyctl 操作的句柄解析入口：正序列号走 `key_lookup()` 红黑树查找，负的 `KEY_SPEC_*` 走 switch 分支。session keyring 分支有自动安装语义：

```c
// process_keys.c:666 KEY_SPEC_SESSION_KEYRING 分支（节选）
if (!ctx.cred->session_keyring) {
	ret = look_up_user_keyrings(NULL, &user_session);
	if (lflags & KEY_LOOKUP_CREATE)
		ret = join_session_keyring(NULL);
	else
		ret = install_session_keyring(user_session);
	...
	goto reget_creds;
}
```

即首次访问 session keyring 时自动挂上用户会话 keyring（`KEY_LOOKUP_CREATE` 则新建私有会话）；安装后 `goto reget_creds` 重取 cred 再来一遍。`KEY_SPEC_REQUESTOR_KEYRING` 从当前 authkey 的 `rka->dest_keyring` 取——这正是 `/sbin/request-key` helper 能把新 key 放进请求者 keyring 的机制。返回的 `key_ref_t` 低位携带 possessed 标志（`make_key_ref(key, 1)`），供后续 possessor 权限叠加使用。

**特殊 keyring**：`.builtin_trusted`/`.secondary_trusted`（内核密钥）、`.platform`/`.machine`/`.evm`/`.ima`（完整性）、`.blacklist`（模块黑名单）、`.persistent`（用户持久）等，由 `CONFIG_*` 决定。

## 7. 当前配置下的状态

- `CONFIG_CRYPTO=n`：`crypto/` 不编译；`/proc/crypto` 不存在；`AF_ALG`（`CONFIG_CRYPTO_USER_API*`）不可用；任何依赖 crypto 的子系统（dm-crypt、IPsec、Wi-Fi 加密、IMA 哈希）都无法启用。
- `CONFIG_KEYS=n`：`security/keys/` 不编译；`add_key`/`request_key`/`keyctl` 系统调用不存在（调用返回 `-ENOSYS`）；`key->security` LSM blob 与 `security_key_*` hook 也随之消失。
- 由于 `CONFIG_KEYS` 关闭，`CONFIG_ENCRYPTED_KEYS`、`CONFIG_TRUSTED_KEYS`、`CONFIG_ASYMMETRIC_KEY_TYPE` 及其下游（模块签名验证 `CONFIG_MODULE_SIG` 的部分路径、`CONFIG_INTEGRITY` 验签）在本构建中不可用。
- 若需在本机构建启用，最小集合：`CONFIG_CRYPTO=y`、`CONFIG_CRYPTO_MANAGER=y`、`CONFIG_CRYPTO_HASH/SKCIPHER/AEAD/...` 以及具体算法；密钥侧 `CONFIG_KEYS=y`，按需加 `CONFIG_KEY_DH_OPERATIONS`、`CONFIG_BIG_KEYS`、`CONFIG_ENCRYPTED_KEYS`、`CONFIG_TRUSTED_KEYS`、`CONFIG_ASYMMETRIC_KEY_TYPE`。
- 需要从用户态使用 crypto 时还要打开 `CONFIG_CRYPTO_USER_API`、`CONFIG_CRYPTO_USER_API_HASH`、`CONFIG_CRYPTO_USER_API_SKCIPHER`、`CONFIG_CRYPTO_USER_API_AEAD`、`CONFIG_CRYPTO_USER_API_RNG`。

## 8. key 生命周期与垃圾回收

key 的引用计数是 `key->usage`，但释放不是即时的：

```c
// key.c:647 key_put()
if (refcount_dec_and_test(&key->usage)) {
	if (test_bit(KEY_FLAG_IN_QUOTA, &key->flags)) {
		spin_lock_irqsave(&key->user->lock, flags);
		key->user->qnkeys--;
		key->user->qnbytes -= key->quotalen;
		spin_unlock_irqrestore(&key->user->lock, flags);
	}
	clear_bit_unlock(KEY_FLAG_USER_ALIVE, &key->flags);
	schedule_work(&key_gc_work);
}
```

`key_put()` 只归还配额、清 `KEY_FLAG_USER_ALIVE` 并调度 GC work——它不睡眠，可在任何上下文调用。真正的销毁由 `key_garbage_collector()`（`gc.c:178`）在进程上下文完成：

- 遍历 `key_serial_tree` 红黑树（只有该函数被允许从树中摘除节点），把失去 `USER_ALIVE` 的 key 收进静态 `graveyard` 链表；
- 状态机分多轮推进：`KEY_GC_REAPING_LINKS`（清理过期链接）→ `REAPING_DEAD_1/2/3`（类型注销时的标记、断链、销毁三轮）；
- 销毁前 `synchronize_rcu()` 确保没有 RCU 读者仍引用 key；
- `key_gc_unused_keys()`（`gc.c:135`）调 `type->destroy()` 释放 payload、`security_key_free()` 释放 LSM blob，最后 **`memzero_explicit(key, sizeof(*key))`** 再归还 `key_jar` slab——key 结构可能承载过敏感数据，必须显式清零。

**revoke 与 invalidate 的区别**：`key_revoke()`（`key.c:1131`）置 `KEY_FLAG_REVOKED`、调 `type->revoke()`，并在 `key_gc_delay`（默认 300 秒，`gc.c:16`）后调度 GC；`key_invalidate()`（`key.c:1167`）置 `KEY_FLAG_INVALIDATED` 并立即调度链接清理——被 invalidate 的 key 从此被所有搜索忽略，错误是 `-ENOKEY` 而非 `-EKEYREVOKED`。过期由 `key_set_expiry()` + 定时器（`key_gc_timer_func`）驱动；`KEY_TYPE_INSTANT_REAP` 类型到期即回收，不等 delay。

**类型注销**：`unregister_key_type()` 调 `key_gc_keytype()`（`gc.c:112`），把该类型所有 key 标记 DEAD 并同步等待三轮 GC 完成——保证模块卸载时不残留该类型的 key。

## 9. request_key 上升调用详解

`request_key_and_link()`（`request_key.c:574`）的完整流程：

```
request_key_and_link(type, desc, callout_info, dest_keyring)
  ├─ type->match_preparse()          自定义匹配（如 dns_resolver）
  ├─ check_cached_key()              每任务缓存最近一次成功的请求
  ├─ search_process_keyrings_rcu()   依次搜 thread/process/session keyring
  ├─ 命中 → key_link(dest_keyring) + cache_requested_key()
  └─ -EAGAIN（可搜索但未找到）且有 callout_info
       └─ construct_key_and_link()
            ├─ construct_alloc_key() 分配 UNINSTANTIATED key，置 USER_CONSTRUCT
            ├─ construct_key()
            │    ├─ request_key_auth_new() 生成 request_key_auth 类型 authkey
            │    └─ actor = key->type->request_key ?: call_sbin_request_key
            └─ wait_for_key_construction() 等待 USER_CONSTRUCT 位
```

`call_sbin_request_key()`（`request_key.c:118`）是默认 actor：

1. 分配名为 `_req.<serial>` 的临时 session keyring，把 authkey 链接进去；
2. 以 `call_usermodehelper_keys(..., UMH_WAIT_PROC)` 执行 `/sbin/request-key <op> <key-serial> <uid> <gid> <thread-kr> <process-kr> <session-kr>`，临时 keyring 成为 helper 进程的 session keyring；
3. helper（通常是 keyutils 按 request-key.conf 规则）通过 `keyctl_assume_authority` 取得 authkey，再 `keyctl_instantiate` 填充目标 key，或 `keyctl_negate` 拒绝；
4. 成功时 `__key_instantiate_and_link()` 清 `USER_CONSTRUCT` 并 `wake_up_bit()` 唤醒等待者；失败时 `key_reject_and_link()` 把负错误码写入 `key->state`，等待者得到 `-ENOKEY`。

authkey 是一次性凭证：记录目标 key、callout_info 与目的 keyring，实例化完成后立即 `key_invalidate(authkey)` 作废，防止重放。`wait_for_key_construction()`（`request_key.c:676`）用 `wait_on_bit(&key->flags, KEY_FLAG_USER_CONSTRUCT, ...)`，随后检查 `key_read_state()` 与 `key_validate()`——negative key、revoked、expired 在此区分。

## 10. trusted 与 encrypted keys

这两类 key 构成内核的“密钥层级”：trusted key 以硬件（TPM）为根，encrypted key 以 trusted key（或 user key）为根。

### 10.1 trusted keys（`security/keys/trusted-keys/trusted_core.c`）

payload 是 datablob 文本命令，`datablob_parse()` 解析出三种操作：

| 命令 | 行为 |
|------|------|
| `new [keylen]` | `trusted_key_get_random` 生成随机 key → `trusted_key_seal` 密封 |
| `load <blob>` | `trusted_key_unseal` 解封已有 blob |
| `update` | 用新 PCR 值重新密封（`trusted_update`） |

seal/unseal 后端经 static_call 分发（TPM 1.2 / TPM 2.0，`trusted_tpm1.c`/`trusted_tpm2.c`），密封 blob 绑定 TPM 的 SRK 与 PCR 策略。**`trusted_read()` 只返回密封 blob**，绝不返回明文；明文只存在于内核的 `trusted_key_payload`，实例化失败与销毁都用 `kfree_sensitive()`。

### 10.2 encrypted keys（`security/keys/encrypted-keys/encrypted.c`）

master key 来自 `trusted:<desc>` 或 `user:<desc>` 前缀（`request_master_key()`）；实际加密密钥由 `get_derived_key()`（`encrypted.c:331`）派生：

```c
derived_buf_len = strlen("AUTH_KEY") + 1 + master_keylen;
...
memcpy(derived_buf + strlen(derived_buf) + 1, master_key, master_keylen);
sha256(derived_buf, derived_buf_len, derived_key);
```

即 SHA256("ENC_KEY"\0 + master) 与 SHA256("AUTH_KEY"\0 + master) 分别派生加密密钥与认证密钥，payload 用 `cbc(aes)`（`blkcipher_alg`，`encrypted.c:40`）加密。支持 `default`/`ecryptfs`（eCryptfs 用）/`enc32`（固定 32 字节 payload）三种格式。`user_decrypted_data` 模块参数（默认取决于 `CONFIG_USER_DECRYPTED_DATA`）控制是否允许用户态实例化时直接提供明文。

### 10.3 与 EVM 的组合

EVM 的 HMAC 密钥正是一个名为 `evm-key` 的 encrypted key（见 [03-ima-evm.md](03-ima-evm.md)）：`evm_init_key()` `request_key()` 取它、用解密明文、随即清零。信任链为：TPM SRK → trusted key → encrypted key（evm-key）→ EVM HMAC → `security.evm` → 文件元数据完整性。任何一环缺失（无 TPM、密钥未加载），EVM 只能走签名模式或不启用。

## 11. keyctl 系统调用族

三个系统调用入口都在 `security/keys/keyctl.c`：`add_key`（`:74`）、`request_key`（`:167`）、`keyctl`（`:1886`）。`keyctl` 是一个多路复用调用，按 `option` 分发：

| 分组 | 操作 |
|------|------|
| 查找与读取 | `GET_KEYRING_ID`、`SEARCH`、`READ`、`DESCRIBE`、`GET_SECURITY` |
| 生命周期 | `UPDATE`、`REVOKE`、`INVALIDATE`、`SET_TIMEOUT` |
| keyring 结构 | `JOIN_SESSION_KEYRING`、`CLEAR`、`LINK`、`UNLINK`、`MOVE`、`SESSION_TO_PARENT` |
| 属主与权限 | `CHOWN`、`SETPERM` |
| 构造（helper 用） | `INSTANTIATE`、`INSTANTIATE_IOV`、`NEGATE`、`REJECT`、`ASSUME_AUTHORITY` |
| 请求策略 | `SET_REQKEY_KEYRING`、`GET_PERSISTENT`、`RESTRICT_KEYRING` |
| 密码运算 | `DH_COMPUTE`、`PKEY_QUERY`、`PKEY_ENCRYPT`、`PKEY_DECRYPT`、`PKEY_SIGN`、`PKEY_VERIFY` |
| 其它 | `CAPABILITIES`、`WATCH_KEY`（`CONFIG_KEY_NOTIFICATIONS`） |

值得注意的几点：

- `PKEY_*` 把非对称运算直接暴露给用户态（底层走 crypto akcipher/sig 前端），`DH_COMPUTE` 对应 `CONFIG_KEY_DH_OPERATIONS`；
- `RESTRICT_KEYRING` 给 keyring 挂 `key_restriction`，限制后续能链接进来的 key（IMA/EVM 密钥环的链接限制即由此而来，见 [03-ima-evm.md](03-ima-evm.md)）；
- `GET_PERSISTENT` 按 UID 取持久 keyring，过期时间由 `/proc/sys/kernel/keys/persistent_keyring_expiry` 控制；
- `WATCH_KEY` 配合 `CONFIG_KEY_NOTIFICATIONS` 允许用户态 watch key 状态变化（实例化/revoke/invalidate），GC 销毁前会 `remove_watch_list()`。

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
| `crypto/algboss.c` | cryptomgr：`cryptomgr_probe` 内核线程、模板实例化与自测调度 |
| `crypto/af_alg.c` | AF_ALG 核心：`alg_bind`、`alg_setsockopt`、`af_alg_accept`、`af_alg_sendmsg` |
| `crypto/algif_skcipher.c` `algif_aead.c` `algif_hash.c` `algif_rng.c` | AF_ALG 具体类型 |
| `include/uapi/linux/if_alg.h` | `sockaddr_alg`、`ALG_SET_*` |
| `include/linux/key.h` | `struct key`、`keyring_index_key`、权限位、`enum key_need_perm` |
| `include/linux/key-type.h` | `struct key_type` 及其回调 |
| `include/uapi/linux/keyctl.h` | `KEY_SPEC_*`、`KEYCTL_*` |
| `security/keys/key.c` | `key_alloc()`、`key_create_or_update()`、配额与序列号 |
| `security/keys/keyring.c` | keyring 的 `assoc_array` 索引、`keyring_search()`、`keyring_alloc()` |
| `security/keys/gc.c` | `key_garbage_collector()`、graveyard、`key_gc_delay`、类型注销回收 |
| `security/keys/request_key_auth.c` | `request_key_auth` 类型与 authkey 生成 |
| `security/keys/permission.c` | `key_task_permission()` |
| `security/keys/process_keys.c` | thread/process/session keyring 安装与 `lookup_user_key()` |
| `security/keys/request_key.c` | `request_key_and_link()`、`/sbin/request-key` 上升调用 |
| `security/keys/user_defined.c` | `user`/`logon` key 类型 |
| `security/keys/big_key.c` `encrypted-keys/` `trusted-keys/` | 大负载、加密、TPM 可信 key |
| `security/keys/keyctl.c` | `add_key`/`request_key`/`keyctl` 系统调用入口 |
| `crypto/asymmetric_keys/asymmetric_type.c` | `asymmetric` key 类型（供验签使用） |
