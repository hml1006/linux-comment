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
