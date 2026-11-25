# 无线与蓝牙子系统：cfg80211 / mac80211 / Bluetooth

> 本文补齐有线网络之外的无线协议栈入口。重点说明用户态配置接口、内核抽象层、驱动边界，以及数据/管理帧的基本路径。
>
> 当前 `.config` 未启用 `CFG80211`/`MAC80211`/`BT`，下文描述的是 Linux 7.2 源码能力。

---

## 1. 无线栈分层

```
用户态：iw / wpa_supplicant / hostapd / NetworkManager
  │ nl80211 (netlink)
cfg80211（net/wireless）
  │ 设备/接口/扫描/连接/认证/密钥管理抽象
mac80211（net/mac80211）
  │ 802.11 协议栈：MLME、TX/RX、聚合、加密、电源管理
具体无线驱动（drivers/net/wireless/...）
  │
硬件（Wi-Fi MAC/PHY）
```

两个关键分工：

- **cfg80211**：面向用户态的配置面（nl80211），管理 `wiphy`、`wireless_dev`、扫描请求、连接状态、监管域；
- **mac80211**：面向软件的 802.11 MAC 层，驱动可以选择“全硬件 MAC”或“mac80211 软件 MAC”。

---

## 2. cfg80211

### 2.1 核心对象

| 对象 | 作用 |
|---|---|
| `struct wiphy` | 一个无线硬件（PHY）的能力：频段、速率、加密、接口模式 |
| `struct wireless_dev` | 一个网络接口（STA/AP/P2P/IBSS） |
| `struct cfg80211_registered_device` | wiphy 与 rdev 的注册实例 |
| `struct cfg80211_scan_request` | 扫描请求 |
| `struct cfg80211_connect_params` | 连接参数（SSID、BSSID、密钥、auth） |

### 2.2 nl80211 命令路径

```
iw dev wlan0 scan
  → netlink: NL80211_CMD_TRIGGER_SCAN
  → cfg80211_scan()
  → rdev_scan()
  → 驱动 scan 回调
  → 驱动上报 cfg80211_scan_done()
  → netlink: NL80211_CMD_NEW_SCAN_RESULTS
```

常见命令：

| 命令 | 作用 |
|---|---|
| `NL80211_CMD_GET_WIPHY` | 查询 PHY 能力 |
| `NL80211_CMD_TRIGGER_SCAN` | 触发扫描 |
| `NL80211_CMD_CONNECT` | STA 连接 |
| `NL80211_CMD_START_AP` | 启动 AP |
| `NL80211_CMD_NEW_KEY` | 安装密钥 |
| `NL80211_CMD_SET_CHANNEL` | 设置信道 |

### 2.3 关键源码

| 文件 | 作用 |
|---|---|
| `net/wireless/core.c` | wiphy/接口注册 |
| `net/wireless/nl80211.c` | nl80211 命令分发 |
| `net/wireless/scan.c` | 扫描与 BSS 管理 |
| `net/wireless/sme.c` | 连接状态机 |
| `net/wireless/util.c` | 信道/速率/IE 工具 |
| `include/net/cfg80211.h` | cfg80211 核心结构 |

### 2.4 源码核对：`wiphy_register` 与扫描全流程

`net/wireless/core.c` 的 `wiphy_register()` 是"一块无线硬件进入内核"的总入口：它校验 `wiphy` 的能力/接口组合、创建 sysfs 与 debugfs 节点、注册 regulatory、初始化 `wiphy.mtx` 与 BSS 表。此后**所有 nl80211 命令都在 `wiphy_lock()`（即 `rdev->wiphy.mtx`）保护下执行**：

```c
static inline void wiphy_lock(struct wiphy *wiphy)
{
	mutex_lock(&wiphy->mtx);
}
```

扫描路径的关键函数链（本树源码）：

```c
/* net/wireless/scan.c */
int cfg80211_scan(struct cfg80211_registered_device *rdev)
{
	...
	if (!(rdev->wiphy.flags & WIPHY_FLAG_SPLIT_SCAN_6GHZ)) {
		rdev_req->req.first_part = true;
		return rdev_scan(rdev, rdev_req);
	}
	...
}

void ___cfg80211_scan_done(struct cfg80211_registered_device *rdev,
			   bool send_message)
{
	lockdep_assert_held(&rdev->wiphy.mtx);
	...
	if (wdev->netdev)
		cfg80211_sme_scan_done(wdev->netdev);

	if (!request->info.aborted &&
	    request->req.flags & NL80211_SCAN_FLAG_FLUSH) {
		/* flush entries from previous scans */
		spin_lock_bh(&rdev->bss_lock);
		__cfg80211_bss_expire(rdev, request->req.scan_start);
		spin_unlock_bh(&rdev->bss_lock);
	}

	msg = nl80211_build_scan_msg(rdev, wdev, request->info.aborted);
	...
}
```

要点：

1. **扫描请求对象**是 `cfg80211_scan_request_int`（`net/wireless/core.h`），它把公开的 `cfg80211_scan_request` 包一层，内部保存 `rdev->int_scan_req`，供 6 GHz 分段扫描（`WIPHY_FLAG_SPLIT_SCAN_6GHZ`）复用。
2. **扫描完成可能分两段**：`___cfg80211_scan_done()` 发现还有 6 GHz PSC 段未扫时，会先发起第二段并直接返回，不向用户态发结果。
3. **BSS 表由 `rdev->bss_lock`（spinlock，`_bh` 变体）保护**，`cfg80211_scan_done()` 的调用者可能处于软中断上下文（驱动在 NAPI/任务中回报），所以必须用 `_bh`。
4. 扫描结果最终由 `nl80211_build_scan_msg()` 组装成 `NL80211_CMD_NEW_SCAN_RESULTS`，通过 netlink 广播给 `wpa_supplicant`。
5. `cfg80211_sme_scan_done()` 是"扫描完成后自动重连"的挂钩——cfg80211 自己实现了 SME（Station Management Entity）时用它推进状态机。

---

## 3. mac80211

### 3.1 核心对象

| 对象 | 作用 |
|---|---|
| `struct ieee80211_hw` | 驱动注册的硬件实例 |
| `struct ieee80211_vif` | 虚拟接口（STA/AP/AP_VLAN/IBSS/MESH） |
| `struct ieee80211_sta` | 对端站点（AP 模式下的客户端） |
| `struct ieee80211_key` | 加密密钥 |
| `struct ieee80211_tx_control` | 发送控制信息 |

### 3.2 发送路径

```
内核网络栈
  → ieee80211_subif_start_xmit()
  → ieee80211_tx()
       ├─ 选择 vif/sta
       ├─ 加密（CCMP/GCMP/TKIP/WEP）
       ├─ 聚合（A-MPDU/A-MSDU）
       └─ 驱动 tx 回调（ieee80211_ops->tx）
  → 硬件发送
  → tx 完成回调 ieee80211_tx_status()
```

### 3.3 接收路径

```
硬件中断/NAPI
  → 驱动 rx 回调 ieee80211_rx()
  → ieee80211_rx_monitor() / ieee80211_rx_handlers()
       ├─ 解密
       ├─ 去聚合
       ├─ 管理帧处理（beacon/probe/auth/assoc）
       └─ 数据帧上送
  → netif_receive_skb()
```

### 3.4 MLME 与状态机

mac80211 的 MLME 负责：

- 扫描结果选择 BSS；
- auth/assoc 帧交互；
- 4-way handshake 后的密钥安装；
- 断开/重连、roaming；
- AP 模式的 beacon、客户端管理、功率管理。

关键源码：

| 文件 | 作用 |
|---|---|
| `net/mac80211/main.c` | mac80211 注册 |
| `net/mac80211/tx.c` | 发送路径 |
| `net/mac80211/rx.c` | 接收路径 |
| `net/mac80211/mlme.c` | STA 模式 MLME |
| `net/mac80211/cfg.c` | cfg80211 回调 |
| `net/mac80211/key.c` | 密钥管理 |
| `net/mac80211/agg-tx.c` / `agg-rx.c` | 聚合 |
| `include/net/mac80211.h` | 驱动接口 |

### 3.5 源码核对：`ieee80211_alloc_hw_nm()` 的对象布局

mac80211 驱动调用的第一个函数是 `ieee80211_alloc_hw_nm()`（`net/mac80211/main.c`）。它先做一组"驱动必须满足"的断言，再一次性分配 `wiphy + ieee80211_local + 驱动私有数据`：

```c
	if (WARN_ON(!ops->tx || !ops->start || !ops->stop || !ops->config ||
		    !ops->add_interface || !ops->remove_interface ||
		    !ops->configure_filter || !ops->wake_tx_queue))
		return NULL;
	...
	/*
	 * +-------------------------+
	 * | struct wiphy	    |
	 * +-------------------------+
	 * | struct ieee80211_local  |
	 * +-------------------------+
	 * | driver's private data   |
	 * +-------------------------+
	 */
	priv_size = ALIGN(sizeof(*local), NETDEV_ALIGN) + priv_data_len;

	wiphy = wiphy_new_nm(&mac80211_config_ops, priv_size, requested_name);
```

两个容易踩的点：

1. **`wake_tx_queue` 是强制回调**。mac80211 的 TX 是"队列 + 唤醒"模型：它把帧排进 `ieee80211_txq`，然后调用 `ieee80211_ops->wake_tx_queue` 通知驱动来取。驱动若没有实现（或没有链接 `ieee80211_handle_wake_tx_queue()`），`alloc_hw` 直接返回 NULL，表现为 `probe` 失败而没有明显错误日志。
2. **内存布局是连续的**。`local` 与驱动私有数据都在 `wiphy->priv` 之后，`hw_to_local()` / `hw->priv` 只是指针运算，因此驱动不能用 `devm_kzalloc` 单独分配 `ieee80211_hw`。

`ieee80211_register_hw()` 接着做能力校验：接口组合、band/速率、MLO（`WIPHY_FLAG_SUPPORTS_MLO` 要求 `link_info_changed`、`MFP_CAPABLE`、`AP_LINK_PS` 等一串回调）、`netdev_features` 白名单。校验失败同样返回 `-EINVAL`，而不是"部分可用"。

### 3.6 认证/关联：cfg80211 与 mac80211 的分界

`iw wlan0 connect` 触发的 `NL80211_CMD_CONNECT` 在 `nl80211_connect()` 里被拆成若干步，其中认证/关联两个回调由 cfg80211 的 `cfg80211_mlme_auth()`/`cfg80211_mlme_assoc()` 发起。mac80211 在 `net/mac80211/cfg.c` 里注册了它们：

```c
	.scan = ieee80211_scan,
	...
	.auth = ieee80211_auth,
	.assoc = ieee80211_assoc,
	.deauth = ieee80211_deauth,

static int ieee80211_auth(struct wiphy *wiphy, struct net_device *dev,
			  struct cfg80211_auth_request *req)
{
	return ieee80211_mgd_auth(IEEE80211_DEV_TO_SUB_IF(dev), req);
}
```

因此完整的 STA 连接路径是：

```
iw / wpa_supplicant
  → NL80211_CMD_CONNECT
  → nl80211_connect()
  → cfg80211_connect() (sme.c, 保存 connect params)
  → cfg80211_mlme_auth()  → rdev_auth()
  → ieee80211_auth()      → ieee80211_mgd_auth()      [mlme.c]
  → 发送 Authentication 帧（驱动 tx）
  ← 收到 Authentication 帧
  → ieee80211_rx_mgmt_auth()                          [mlme.c]
  → sta_info_move_state(sta, IEEE80211_STA_AUTH)
  → cfg80211_mlme_assoc() → ieee80211_assoc() → ieee80211_mgd_assoc()
  → 发送 Association Request / 收到 Response
  → sta_info_move_state(sta, IEEE80211_STA_ASSOC/AUTHORIZED)
  → cfg80211_connect_result() → NL80211_CMD_CONNECT 完成
```

`net/mac80211/mlme.c` 里这几个函数名可直接对照：`ieee80211_mgd_auth()`、`ieee80211_rx_mgmt_auth()`、`ieee80211_auth_challenge()`、`ieee80211_mgd_assoc()`。`IEEE80211_STA_AUTH` / `IEEE80211_STA_AUTHORIZED` 这两个状态切换点，正是"关联成功"与"4 次握手完成、可以收发数据"的分界。

如果驱动自己实现了 `auth/assoc`（FullMAC，如部分 `iwlwifi`/`ath6kl` 固件路径），cfg80211 会直接调驱动回调，mac80211 完全不参与——这就是 SoftMAC 与 FullMAC 在代码上的分岔。

### 3.7 RCU 保护的三类对象

mac80211 RX 路径开头有一段明确注释：

```c
	/*
	 * key references and virtual interfaces are protected using RCU
	 * and this requires that we are in a read-side RCU section during
	 * receive processing
	 */
	rcu_read_lock();
	ieee80211_rx_list(hw, pubsta, skb, &list);
	rcu_read_unlock();
```

| 对象 | 保护方式 | 释放时机 |
|--|--|--|
| `ieee80211_key` | RCU | `synchronize_rcu()` 后 `kfree`（`key.c`） |
| `ieee80211_sub_if_data`（vif） | RCU（`list_add_tail_rcu` 挂到 `local->interfaces`） | 接口删除时 `synchronize_rcu()` |
| `sta_info` | RCU（`sta_info_hash` + `sta_info_move_state`） | `sta_info_free()` 延迟释放 |

`ieee80211_rx_napi()` 只在 RX 处理期间持读锁，随后 `netif_receive_skb_list()`/`napi_gro_receive()` 在锁外执行——这是为了避免把网络栈的长时间处理放进 RCU 临界区。

---

## 4. 无线驱动边界

驱动通过 `struct ieee80211_ops` 向 mac80211 注册回调：

```c
static const struct ieee80211_ops my_ops = {
    .tx          = my_tx,
    .start       = my_start,
    .stop        = my_stop,
    .add_interface = my_add_interface,
    .config      = my_config,
    .bss_info_changed = my_bss_info_changed,
    .set_key     = my_set_key,
    .sta_add     = my_sta_add,
    .ampdu_action = my_ampdu_action,
    .sw_scan_start = my_sw_scan_start,
};
```

驱动只需处理硬件相关的 TX/RX、寄存器、固件下载；802.11 协议状态由 mac80211 维护。

### 4.1 与 netdev 的关系：vif 就是一个 net_device

无线接口在 Linux 里仍然是 `struct net_device`，只是额外挂了一个 `wireless_dev`：

```c
/* net/mac80211/iface.c, ieee80211_if_add() */
	ndev->min_mtu = 256;
	if (type == NL80211_IFTYPE_MONITOR)
		ndev->max_mtu = 0;
	else
		ndev->max_mtu = local->hw.max_mtu;

	ret = cfg80211_register_netdevice(ndev);
```

```c
/* net/wireless/core.c */
int cfg80211_register_netdevice(struct net_device *dev)
{
	struct wireless_dev *wdev = dev->ieee80211_ptr;
	...
	ASSERT_RTNL();
	lockdep_assert_held(&rdev->wiphy.mtx);
	...
	ret = register_netdevice(dev);
	if (ret)
		goto out;

	cfg80211_register_wdev(rdev, wdev);
	...
}
```

由此可以推出几个实用结论：

| 现象 | 原因 |
|--|--|
| `ip link` 能看到 `wlan0` | vif 是标准 netdev，走 `register_netdevice()` |
| 改接口类型要 `iw dev wlan0 set type` | 新类型意味着 `ieee80211_setup_sdata()` 重建 `u.mgd/u.ap/u.ibss` 联合体 |
| 加接口需要 `ip link add wlan1 type __ap` | `ieee80211_if_add()` 会新建 netdev 并调用 `cfg80211_register_netdevice()` |
| 抓包用 `monitor` 接口 | `NL80211_IFTYPE_MONITOR` 时 `max_mtu = 0`（不限制，因为可能是 A-MSDU/MPDU） |
| 注册时要求 RTNL + wiphy 锁 | `ASSERT_RTNL()` 与 `lockdep_assert_held(&rdev->wiphy.mtx)` 双重保护 |

数据路径上的"协议转换"发生在 `ieee80211_subif_start_xmit()` / `__ieee80211_subif_start_xmit()`：网络栈交给驱动的是 802.3 帧（带以太网头），mac80211 负责加 802.11 头、选速率、加密、聚合，再交给 `ieee80211_ops->tx`。接收方向相反，`ieee80211_rx_list()` 解出 802.3 后通过 `netif_receive_skb_list()` 交回网络栈。

---

## 5. Bluetooth

### 5.1 分层

```
用户态：bluetoothd / BlueZ
  │ HCI socket / mgmt socket
Bluetooth core（net/bluetooth）
  ├─ HCI：命令/事件/ACL/SCO 传输
  ├─ L2CAP：逻辑链路复用
  ├─ SMP：配对与密钥
  ├─ RFCOMM：串口仿真
  ├─ SCO/eSCO：语音
  └─ mgmt：控制器管理
具体传输驱动（USB/UART/SDIO）
```

### 5.2 核心对象

| 对象 | 作用 |
|---|---|
| `struct hci_dev` | 一个蓝牙控制器 |
| `struct hci_conn` | 一条 ACL/SCO 连接 |
| `struct l2cap_chan` | L2CAP 通道 |
| `struct sco_conn` | SCO 连接 |

### 5.3 HCI 命令路径

```
bluetoothctl power on
  → mgmt socket: MGMT_OP_SET_POWERED
  → hci_dev_do_open()
  → hci_cmd_sync(HCI_OP_RESET)
  → 驱动 send 回调（hdev->send）
  → 控制器事件 hci_recv_frame()
  → hci_event_packet()
```

### 5.4 关键源码

| 文件 | 作用 |
|---|---|
| `net/bluetooth/hci_core.c` | HCI 核心 |
| `net/bluetooth/hci_event.c` | HCI 事件处理 |
| `net/bluetooth/l2cap_core.c` | L2CAP |
| `net/bluetooth/smp.c` | 配对 |
| `net/bluetooth/rfcomm/` | RFCOMM |
| `net/bluetooth/mgmt.c` | mgmt 接口 |
| `drivers/bluetooth/` | USB/UART/SDIO 传输驱动 |

### 5.5 源码核对：`hci_register_dev()` 做了什么

传输驱动（USB/UART/SDIO）在探测成功后调用 `hci_register_dev()`（`net/bluetooth/hci_core.c`），它完成"控制器从设备变成 hciN"的全过程：

```c
int hci_register_dev(struct hci_dev *hdev)
{
	int id, error;

	if (!hdev->open || !hdev->close || !hdev->send)
		return -EINVAL;

	id = ida_alloc_max(&hci_index_ida, HCI_MAX_ID - 1, GFP_KERNEL);
	...
	error = dev_set_name(&hdev->dev, "hci%u", id);
	...
	hdev->workqueue = alloc_ordered_workqueue("%s", WQ_HIGHPRI, hdev->name);
	...
	hdev->req_workqueue = alloc_ordered_workqueue("%s", WQ_HIGHPRI, hdev->name);
	...
	device_add(&hdev->dev);
	hci_leds_init(hdev);
	hdev->rfkill = rfkill_alloc(...);
	...
	hci_dev_set_flag(hdev, HCI_SETUP);
	hci_dev_set_flag(hdev, HCI_AUTO_OFF);
	hci_dev_set_flag(hdev, HCI_BREDR_ENABLED);
	...
	hci_sock_dev_event(hdev, HCI_DEV_REG);
	hci_dev_hold(hdev);
	...
	queue_work(hdev->req_workqueue, &hdev->power_on);
```

要点：

1. **三个强制回调**：`open`（上电）、`close`（下电）、`send`（发 HCI 包）。缺任意一个直接 `-EINVAL`。
2. **两个 ordered workqueue**：`workqueue` 处理 RX/事件，`req_workqueue` 处理命令同步与上电流程。ordered 保证"同一 hdev 的命令严格串行"，这是 HCI 协议的基本要求（控制器一次只能处理一条命令）。
3. **注册不立即上电**：只 `queue_work(power_on)`；`hci_power_on()` 再调用 `hci_dev_do_open()` → `hci_dev_open_sync()`。`HCI_AUTO_OFF` 标志让设备注册后保持关闭，等 `bluetoothd` 通过 mgmt 接口显式 `power on`。
4. **编号来自 IDA**：`ida_alloc_max(&hci_index_ida, HCI_MAX_ID - 1, ...)`，因此 `hci0/hci1/...` 的编号可复用，卸载后不保证连续。

### 5.6 命令队列与数据路径

HCI 命令不是"调用即发"，而是排进队列由 `cmd_sync_work` 串行执行：

```c
int hci_cmd_sync_submit(struct hci_dev *hdev, hci_cmd_sync_work_func_t func,
			void *data, hci_cmd_sync_work_destroy_t destroy)
{
	struct hci_cmd_sync_work_entry *entry;
	int err = 0;

	mutex_lock(&hdev->unregister_lock);
	if (hci_dev_test_flag(hdev, HCI_UNREGISTER)) {
		err = -ENODEV;
		goto unlock;
	}

	entry = kmalloc_obj(*entry);
	...
	mutex_lock(&hdev->cmd_sync_work_lock);
	list_add_tail(&entry->list, &hdev->cmd_sync_work_list);
	mutex_unlock(&hdev->cmd_sync_work_lock);

	queue_work(hdev->req_workqueue, &hdev->cmd_sync_work);
unlock:
	mutex_unlock(&hdev->unregister_lock);
	return err;
}

int hci_cmd_sync_queue(struct hci_dev *hdev, hci_cmd_sync_work_func_t func,
		       void *data, hci_cmd_sync_work_destroy_t destroy)
{
	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return -ENETDOWN;

	return hci_cmd_sync_submit(hdev, func, data, destroy);
}
```

- `hci_cmd_sync_queue()` 多一道 `HCI_RUNNING` 检查：设备未打开时返回 `-ENETDOWN`；`hci_cmd_sync_submit()` 供"初始化阶段也要发命令"的调用者使用（此时尚未 RUNNING）。
- 队列元素是 `hci_cmd_sync_work_entry`，执行函数是 `hci_cmd_sync_work_func_t`，返回 `int` 状态；`destroy` 回调负责释放 `data`。

数据方向的两条路：

```c
/* 接收：驱动在中断/NAPI 里调用 */
int hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	...
	skb_queue_tail(&hdev->rx_q, skb);
	queue_work(hdev->workqueue, &hdev->rx_work);
	return 0;
}

/* 发送：hci_send_frame() 里 */
	__net_timestamp(skb);
	hci_send_to_monitor(hdev, skb);
	if (atomic_read(&hdev->promisc))
		hci_send_to_sock(hdev, skb);
	skb_orphan(skb);
	...
	hdev->send(hdev, skb);   /* 最终交给传输驱动 */
```

关键点：

- RX 必须在**进程上下文**处理（`hci_rx_work()`），因为事件处理会睡眠（发命令、等完成），不能直接在中断里做。
- `hci_send_frame()` 先做 monitor/promisc 复制，再 `skb_orphan()`，最后调用驱动的 `send`——这样 `btmon` 与 HCI socket 用户都能看到原始包。
- `hci_recv_frame()` 只接受 `HCI_UP || HCI_INIT` 状态的包，否则丢弃并返回 `-ENXIO`；`HCI_INIT` 阶段仍要收事件，否则初始化无法完成。

### 5.7 蓝牙与 netdev 的关系

与 Wi-Fi 不同，**Bluetooth core 本身不创建 net_device**：

| 层次 | 是否是 netdev | 说明 |
|--|--|--|
| HCI/L2CAP/RFCOMM/SCO | 否 | 走 HCI socket / mgmt socket / RFCOMM tty，不经过网络栈 |
| BNEP（蓝牙以太网仿真） | 是 | `net/bluetooth/bnep/`，创建 `bnep0`，桥接到 IP |
| 6LoWPAN over BLE | 是 | `net/bluetooth/6lowpan.c`，创建 `bt0` |
| HID（hidp / HoG） | 否 | 内核 `hidp` 走 L2CAP 直连 HID，HoG 由 BlueZ 经 `uhid` 实现 |

因此"蓝牙设备在 `ip link` 里看不见"是正常的；只有 BNEP/6LoWPAN 这类"把蓝牙当链路层"的 profile 才会注册 netdev，并把包交给 `netif_rx()`。这也解释了为什么蓝牙的"网络配置"通常由 `bluetoothd` + `bnep` 服务完成，而不是 NetworkManager 直接管理 hci0。

---

## 6. 配置与调试

| 功能 | 配置项 | 用户态工具 |
|---|---|---|
| cfg80211 | `CONFIG_CFG80211` | `iw`、`wpa_supplicant` |
| mac80211 | `CONFIG_MAC80211` | `iw`、`hostapd` |
| 无线驱动 | `CONFIG_*_WIFI` | `dmesg`、`iw phy` |
| Bluetooth | `CONFIG_BT` | `bluetoothctl`、`hcitool` |
| HCI 调试 | `CONFIG_BT_HCIVHCI`/`btmon` | `btmon` |
| 无线调试 | `CONFIG_MAC80211_DEBUGFS` | `/sys/kernel/debug/ieee80211` |

当前 `.config` 中 `CFG80211`/`MAC80211`/`BT` 均未启用，因此上述接口不可用；需要无线功能时先打开对应配置。

---

## 7. 关键文件清单

| 文件 | 作用 |
|---|---|
| `net/wireless/core.c` | cfg80211 核心 |
| `net/wireless/nl80211.c` | nl80211 |
| `net/wireless/scan.c` | 扫描 |
| `net/wireless/sme.c` | 连接状态机 |
| `net/mac80211/main.c` | mac80211 核心 |
| `net/mac80211/tx.c` | 发送 |
| `net/mac80211/rx.c` | 接收 |
| `net/mac80211/mlme.c` | MLME |
| `net/mac80211/cfg.c` | cfg80211 回调 |
| `net/bluetooth/hci_core.c` | HCI 核心 |
| `net/bluetooth/l2cap_core.c` | L2CAP |
| `net/bluetooth/mgmt.c` | mgmt |
| `drivers/net/wireless/` | 无线驱动 |
| `drivers/bluetooth/` | 蓝牙传输驱动 |
