# USB 子系统（主机侧）：分层、枚举、URB、HCD/xHCI 与设备类驱动

> 基于本仓库 Linux 7.2.0 源码（`Makefile`: `VERSION=7 PATCHLEVEL=2`）逐文件核对，文中 `文件:行号` 均可直接定位，代码块为真实源码的精简摘录。
>
> 配置提醒：当前 `.config:2681` 为 `# CONFIG_USB_SUPPORT is not set`，而 `drivers/usb/` 下存在历史编译产物（`ehci-hcd.o`、`libcomposite.ko` 等）。本笔记以源码为准；凡涉及是否编译生效处会注明"源码存在，当前配置未启用"。

## 1. 定位与分层

### 1.1 协议栈描述的对象

USB 是"主机提问、设备回答"的总线协议，设备用层次化描述符自我描述；`include/linux/usb.h:32-46` 的注释直接给出这棵树：设备有一个或多个配置，配置有一个或多个接口，接口有一到多个 altsetting，每个 setting 有零到多个端点，SuperSpeed 端点还带 companion 描述符。

| 概念 | 含义 | 内核对象 |
|------|------|----------|
| Device | 物理设备，拥有唯一 7 位地址 devnum | `struct usb_device` |
| Configuration | 一种工作形态，同一时刻只有一套生效 | `struct usb_host_config` |
| Interface | 一个"功能"（音频流、串口、存储），驱动绑定于此 | `struct usb_interface` |
| Altsetting | 接口的备选设置，用于切换带宽或端点组合 | `struct usb_host_interface` |
| Endpoint | 单向数据通道，0 号端点永远存在 | `struct usb_host_endpoint` |

**USB 驱动不绑定设备，而绑定接口**：`struct usb_driver` 的 probe 原型是 `probe(struct usb_interface *intf, const struct usb_device_id *id)`（`usb.h:1248`）。复合设备会暴露多个 interface，各自匹配不同驱动、互不干扰，这正是"一个设备能同时出现 ttyACM 与 uvcvideo"的原因，也是理解 USB 驱动模型的第一原则。

### 1.2 内核中的四层

```text
        设备侧（gadget）                    主机侧（host）
   ┌───────────────────────┐        ┌────────────────────────────┐
   │ function 驱动          │        │ class/function 驱动         │
   │ f_acm f_mass_storage  │        │ usb-storage cdc_acm hid    │
   └──────────┬────────────┘        └────────────┬───────────────┘
              │ usb_request                      │ urb
   ┌──────────▼────────────┐        ┌────────────▼───────────────┐
   │ composite / configfs  │        │ usbcore                     │
   │ libcomposite          │        │ usb.c message.c urb.c       │
   └──────────┬────────────┘        │ driver.c hub.c config.c    │
              │                     └────────────┬───────────────┘
   ┌──────────▼────────────┐        ┌────────────▼───────────────┐
   │ UDC 驱动               │        │ HCD 核心 hcd.c              │
   └──────────┬────────────┘        └────────────┬───────────────┘
   ┌──────────▼────────────┐        ┌────────────▼───────────────┐
   │ 设备控制器硬件          │        │ xhci / ehci / uhci / ohci   │
   └───────────────────────┘        └────────────┬───────────────┘
                                                 │ USB 线缆与端口
```

四层的分工是：**usbcore**（`drivers/usb/core/`）做与硬件无关的模型接入、枚举、配置切换、URB 校验、带宽计算、根 hub 与电源策略；**HCD** 由 `core/hcd.c` 提供框架、`host/*.c` 提供控制器实现，把 URB 翻译成硬件链（QH/TD、TRB、帧列表）；**class 驱动**（`class/`、`storage/`、`serial/`、`misc/`）只关心 interface，用同步 API 或自行提交 URB；**gadget** 是设备侧的反向栈，框架核心在 `gadget/udc/core.c`。主机侧与设备侧共享"提交—完成"的心智模型，只是对象分别叫 URB 与 usb_request。

### 1.3 目录地图（本树 `drivers/usb/` 共 603 个 `.c`）

| 目录 | `.c` 数 | 作用 | 本笔记 |
|------|--------|------|--------|
| `core/` | 24 | usbcore：模型、枚举、URB、hub、配置、usbfs | 第 2–4、6 节 |
| `host/` | 96 | xHCI/EHCI/UHCI/OHCI 与平台 glue | 第 5 节 |
| `gadget/` | 116 | UDC 框架、composite、configfs、function | 第 8 节 |
| `typec/` | 62 | Type-C 连接器类、PD、mux/altmode | 第 8.4 节 |
| `storage/` | 23 | usb-storage 与 UAS | 第 7 节 |
| `serial/` | 61 | usb-serial 及厂商转换芯片 | 提及 |
| `misc/` | 34 | LED、风扇、测试仪等小设备 | — |
| `dwc3/dwc2/chipidea/musb/cdns3` | 27/12/15/20/22 | 双角色控制器驱动 | 提及 |
| `class/`、`mon/` | 5、4 | cdc-acm/usblp；usbmon | 提及、第 9 节 |

`drivers/usb/core/` 合计 27477 行，是整棵 USB 树的"宪法"。`drivers/usb/Makefile` 用 `obj-$(CONFIG_USB) += core/`、`obj-$(CONFIG_USB_XHCI_HCD) += host/` 等行把子目录挂到配置项；`CONFIG_USB_SUPPORT` 是总开关（`drivers/usb/Kconfig:29`，`depends on HAS_IOMEM`，`default y`），未开时整个目录不参与编译。另需注意 `drivers/net/usb/`（usbnet，59 个 `.c`）不在本目录，却是主机侧最大的消费者之一。

## 2. 核心数据结构

### 2.1 端点与接口

`struct usb_host_endpoint`（`usb.h:69`）既是描述符宿主，也是 URB 队列锚点：

```c
struct usb_host_endpoint {
	struct usb_endpoint_descriptor			desc;
	struct usb_ss_ep_comp_descriptor		ss_ep_comp;
	struct usb_ssp_isoc_ep_comp_descriptor		ssp_isoc_ep_comp;
	struct usb_eusb2_isoc_ep_comp_descriptor	eusb2_isoc_ep_comp;
	struct list_head		urb_list;
	void				*hcpriv;
	struct ep_device		*ep_dev;	/* For sysfs info */
	unsigned char *extra;
	int extralen;
	int enabled;
	int streams;
};
```

`urb_list` 是该端点所有在途 URB，由 usbcore 维护、`usb_hcd_link_urb_to_ep()` 挂链；`hcpriv` 是 HCD 私有数据（EHCI 的 QH、xHCI 的 virt_ep 等），注释第 57-59 行要求"带宽仍被占用时必须保留"；`enabled` 为假时提交会被拒；`streams` 是 USB3 bulk streams 数量；`eusb2_isoc_ep_comp` 是较新的 eUSB2 等时 companion，旧资料通常没有。

`struct usb_interface`（245）才是设备模型意义上的"设备"：

```c
struct usb_interface {
	struct usb_host_interface *altsetting;
	struct usb_host_interface *cur_altsetting;
	unsigned num_altsetting;
	struct usb_interface_assoc_descriptor *intf_assoc;
	int minor;
	enum usb_interface_condition condition;
	unsigned unregistering:1;
	unsigned needs_remote_wakeup:1;
	unsigned needs_binding:1;
	unsigned authorized:1;
	enum usb_wireless_status wireless_status;
	struct work_struct wireless_status_work;
	struct device dev;
	struct device *usb_dev;
	struct work_struct reset_ws;
};
```

`condition` 的取值见 `usb.h:99-104`：`UNBOUND → BINDING → BOUND → UNBINDING`，它是排查"驱动已注册却不 probe"的第一手线索。`usb_interface_cache`（354）保存配置未激活时仍需存在的接口信息，比 `usb_interface` 活得久，供 `usb_set_configuration()` 反复切换配置以及 `debugfs/usb/devices` 使用。

### 2.2 配置、总线与设备

`struct usb_host_config`（406）同时持有"活"接口与"缓存"接口：`interface[USB_MAXINTERFACES]` 只在配置生效时有效，`intf_cache[]` 则跟随设备整个生命周期，因此切换配置不会丢描述符。`struct usb_bus`（450）代表一棵设备树而非物理总线，关键成员有 `controller`、`sysdev`、`busnum`、`devmap`（128 位设备号位图）、`root_hub`、`hs_companion`（EHCI 与 OHCI/UHCI 的伴侣关系）以及 `bandwidth_allocated`（周期带宽，单位 us/帧）；`devnum_next` 配 `devnum_next_mutex` 做轮转分配，避免设备号立刻复用。

```c
struct usb_device {
	int		devnum;
	char		devpath[16];
	u32		route;
	enum usb_device_state	state;
	enum usb_device_speed	speed;
	enum usb_ssp_rate	ssp_rate;
	struct usb_tt	*tt;
	int		ttport;
	struct usb_device *parent;
	struct usb_bus *bus;
	struct usb_host_endpoint ep0;
	struct device dev;
	struct usb_device_descriptor descriptor;
	struct usb_host_bos *bos;
	struct usb_host_config *config;
	struct usb_host_config *actconfig;
	struct usb_host_endpoint *ep_in[16];
	struct usb_host_endpoint *ep_out[16];
	char **rawdescriptors;
	unsigned can_submit:1;
	unsigned authorized:1;
	int slot_id;
	unsigned lpm_disable_count;
	u16 hub_delay;
	unsigned use_generic_driver:1;
};
```

这个结构体里最要紧的字段：`devpath`/`route` 是稳定拓扑路径，`route` 是 xHCI route string 且根端口不计入；`state` 只能经 `usb_set_device_state()` 修改（`usb.h:657-660`）；`speed`/`ssp_rate` 由 hub 层在枚举时判定；`tt`/`ttport` 服务于挂在高速 hub 下的低速/全速设备；`ep0` 的 `wMaxPacketSize` 会在枚举中被反复修正；`config` 是全部配置缓存，`actconfig` 是当前配置且会被 `usb_disable_device()` 清空；`ep_in[16]`/`ep_out[16]` 是由 pipe 查端点的快表；`slot_id` 仅 xHCI 使用，0 表示未分配；`use_generic_driver` 表示专用 device driver probe 失败后请求回退到 generic 驱动。

### 2.3 驱动的两种形态

```c
struct usb_driver {                       /* 绑 interface，绝大多数驱动用 */
	const char *name;
	int (*probe) (struct usb_interface *intf, const struct usb_device_id *id);
	void (*disconnect) (struct usb_interface *intf);
	int (*pre_reset) (struct usb_interface *intf);
	int (*post_reset) (struct usb_interface *intf);
	void (*shutdown) (struct usb_interface *intf);
	const struct usb_device_id *id_table;
	struct usb_dynids dynids;
	struct device_driver driver;
	unsigned int supports_autosuspend:1;
};
```

`struct usb_device_driver`（1307）绑定整个 device，字段为 `match/probe/disconnect/suspend/resume/choose_configuration/id_table/generic_subclass`，只有 `usb_generic`、`usbip` 等少数用户。`usb_register_driver()`（`driver.c:1060`）不直接操作总线，而是把 driver core 回调接好再注册：

```c
	new_driver->driver.bus = &usb_bus_type;
	new_driver->driver.probe = usb_probe_interface;
	new_driver->driver.remove = usb_unbind_interface;
	new_driver->driver.shutdown = usb_shutdown_interface;
	new_driver->driver.owner = owner;
	retval = driver_register(&new_driver->driver);
```

总线类型极简（`driver.c:2076`），但 `need_parent_lock = true` 很关键——它让 driver core 在 probe/remove 时先锁父设备（hub），从而实现枚举与驱动绑定的串行化：

```c
const struct bus_type usb_bus_type = {
	.name =		"usb",
	.match =	usb_device_match,
	.uevent =	usb_uevent,
	.need_parent_lock =	true,
};
```

`usb_device_match()`（868）把设备与接口分开匹配：设备只接受 `usb_device_driver`，接口只接受 `usb_driver`，接口侧顺序为 `usb_match_id(id_table)` → `usb_match_dynamic_id(dynids)`。

### 2.4 URB 与 anchor

URB 是主机侧唯一的 I/O 载体（`usb.h:1630`），前半段是核心与 HCD 的私有字段，后半段才是驱动可用的公共字段：

```c
struct urb {
	/* private: usb core and host controller only fields */
	struct kref kref;
	int unlinked;
	void *hcpriv;
	atomic_t use_count;
	atomic_t reject;
	/* public: documented fields */
	struct list_head urb_list;
	struct list_head anchor_list;
	struct usb_anchor *anchor;
	struct usb_device *dev;
	struct usb_host_endpoint *ep;
	unsigned int pipe;
	unsigned int stream_id;
	int status;
	unsigned int transfer_flags;
	void *transfer_buffer;
	dma_addr_t transfer_dma;
	struct scatterlist *sg;
	u32 transfer_buffer_length;
	u32 actual_length;
	unsigned char *setup_packet;
	dma_addr_t setup_dma;
	int interval;
	int number_of_packets;
	int error_count;
	void *context;
	usb_complete_t complete;
	struct usb_iso_packet_descriptor iso_frame_desc[];
};
```

`kref` 管生命周期，`unlinked` 记录取消原因，`hcpriv` 是 HCD 私有数据，`use_count` 与 `reject` 支撑 `usb_kill_urb()`。`usb_anchor`（1425）成批管理 URB，是 disconnect 的"救生圈"：

```c
struct usb_anchor {
	struct list_head urb_list;
	wait_queue_head_t wait;
	spinlock_t lock;
	atomic_t suspend_wakeups;
	unsigned int poisoned:1;
};
```

`usb_anchor_urb()`（126）会额外 `usb_get_urb()` 并挂链；若 anchor 已 poison，则对 URB 的 `reject` 加一，后续提交必失败。

### 2.5 HCD

`struct usb_hcd`（`include/linux/usb/hcd.h:68`）内嵌 `struct usb_bus self`，即"一个 hcd 就是一条 usb_bus"。要点：`driver` 是 `hc_driver` 硬件钩子表；`flags` 含 `HCD_FLAG_HW_ACCESSIBLE/POLL_RH/RH_RUNNING/DEAD`，必须原子位操作；`rh_timer`/`status_urb` 支撑根 hub 轮询；`high_prio_bh`/`low_prio_bh` 是 URB giveback 的两条工作队列通道；`address0_mutex`/`bandwidth_mutex` 分别串行化地址 0 与带宽变更；`shared_hcd`/`primary_hcd` 是 xHCI 的 USB2/USB3 双 hcd 互指；`pool[4]` 是 `hcd_buffer_alloc()` 的 DMA 缓冲池。

`struct hc_driver`（hcd.h:237）是硬件契约，核心只有寥寥几个钩子：

```c
struct hc_driver {
	const char	*description;
	size_t		hcd_priv_size;
	irqreturn_t	(*irq) (struct usb_hcd *hcd);
	int		flags;          /* HCD_USB3 | HCD_BH | ... */
	int	(*reset) (struct usb_hcd *hcd);
	int	(*start) (struct usb_hcd *hcd);
	int	(*urb_enqueue)(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags);
	int	(*urb_dequeue)(struct usb_hcd *hcd, struct urb *urb, int status);
	int	(*hub_control) (struct usb_hcd *hcd, u16 typeReq, u16 wValue,
				u16 wIndex, char *buf, u16 wLength);
};
```

`HCD_BH`（0x0100）决定 completion 是否强制走 BH 上下文，`HCD_USB3` 等决定根 hub 速度；`alloc_dev/free_dev` 现在也不只 xHCI 使用。

## 3. 设备枚举全流程

### 3.1 hub 中断到端口事件

端口变化走"中断 → 位图 → 工作队列"，绝不在中断里枚举：

```text
hub 状态 URB 完成 hub_irq()            hub.c:773
  → 8 字节位图写入 hub->event_bits[0]
  → kick_hub_wq()                      hub.c:708
  → queue_work(hub_wq, &hub->events)
  → hub_event()                        hub.c:5882
  → 对置位端口 port_event()             hub.c:5754
  → 清 C_CONNECTION 等变化位
  → hub_port_connect_change()          hub.c:5645
  → hub_port_connect()                 hub.c:5398
```

`hub_irq()` 对连续 10 次错误只置 `hub->error`，交给 `hub_event()` 里的 `usb_reset_device(hdev)` 复位 hub（hub.c:786-792），避免中断里死循环重试。

### 3.2 端口连接：分配设备并复位

`hub_port_connect()` 的骨架如下（hub.c:5398，精简）：

```c
	if (udev) {
		if (hcd->usb_phy && !hdev->parent)
			usb_phy_notify_disconnect(hcd->usb_phy, udev->speed);
		usb_disconnect(&port_dev->child);
	}
	for (i = 0; i < PORT_INIT_TRIES; i++) {
		usb_lock_port(port_dev);
		mutex_lock(hcd->address0_mutex);
		udev = usb_alloc_dev(hdev, hdev->bus, port1);
		usb_set_device_state(udev, USB_STATE_POWERED);
		udev->bus_mA = hub->mA_per_port;
		udev->level = hdev->level + 1;
		udev->speed = hub_is_superspeed(hub->hdev) ?
				USB_SPEED_SUPER : USB_SPEED_UNKNOWN;
		choose_devnum(udev);
		status = hub_port_init(hub, udev, port1, i, NULL);
		...
		status = usb_new_device(udev);
```

`address0_mutex` 保证"地址 0 上只有一个设备在枚举"——`SET_ADDRESS` 成功前所有设备都用地址 0，这是硬件层面的冲突源。`usb_alloc_dev()`（`usb.c:644`）完成分配、`device_initialize()`、命名（根 hub 为 `usb%d`，子设备为 `"busnum-devpath"`）与 `ep0` 初始化；它会调用 HCD 的 `alloc_dev`（xHCI 在此分配 slot/virt device），但**根 hub 不分配**（usb.c:660 的 `&& parent` 条件）。

### 3.3 复位、取描述符、SetAddress

设备状态迁移如下表，每一步都由 `usb_set_device_state()` 记录：

| 阶段 | `udev->state` | 动作 |
|------|--------------|------|
| 端口上电 | `POWERED` | 打开端口电源，等待 `bPwrOn2PwrGood` |
| 端口复位 | `DEFAULT` | 速度确定，ep0 maxpacket 有初值 |
| 分配地址 | `ADDRESS` | `SET_ADDRESS` 或 HCD 的 `address_device` |
| 安装配置 | `CONFIGURED` | `SET_CONFIGURATION`，接口注册并 probe |
| 拔出 | `NOTATTACHED` | `usb_disconnect()` 递归标记子树 |

`hub_port_reset()`（3058）发起复位，`hub_port_wait_reset()`（2961）最多轮询 `HUB_RESET_TIMEOUT = 800 ms`；完成条件是 **RESET 位清零且 CONNECTION 置位**，因为有些芯片复位后还要时间重建连接（hub.c:2987-2996）。随后读设备描述符，此处与很多旧资料不同：

```c
#define GET_DESCRIPTOR_BUFSIZE	64
	for (i = 0; i < GET_MAXPACKET0_TRIES; ++i) {
		buf->bDescriptorType = buf->bMaxPacketSize0 = 0;
		rc = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
				USB_DT_DEVICE << 8, 0,
				buf, size, initial_descriptor_timeout);
		switch (buf->bMaxPacketSize0) {
		case 8: case 16: case 32: case 64: case 9:
			if (buf->bDescriptorType == USB_DT_DEVICE) {
				rc = buf->bMaxPacketSize0;
				break;
			}
			fallthrough;
		default:
			if (rc >= 0)
				rc = -EPROTO;
			break;
		}
		if (rc > 0 || (rc == -ETIMEDOUT && first_time &&
				udev->speed > USB_SPEED_FULL))
			break;
	}
```

"新方案"（`use_new_scheme()`，2925）先读 64 字节、再复位、再 `SET_ADDRESS`；"旧方案"先 `SET_ADDRESS` 再读 8 字节。源码注释直言：**新方案是 Windows 的做法，能救不合规设备**（5031-5037）。并且 `use_new_scheme()` 对 `speed >= USB_SPEED_SUPER` 直接返回 false——USB3 设备在 Default 状态接收控制命令会导致枚举失败（2922-2933），这条在旧笔记里几乎看不到。重试次数也不像传闻那样固定为 3：`PORT_RESET_TRIES`/`SET_ADDRESS_TRIES`/`GET_DESCRIPTOR_TRIES`/`GET_MAXPACKET0_TRIES` 默认分别为 5/2/2/3，开启 `CONFIG_USB_FEW_INIT_RETRIES` 后降为 2/1/1/1，`PORT_INIT_TRIES` 恒为 4（hub.c:2893-2907）；再叠加 `usb_get_descriptor()` 内部的 3 次重试（`message.c:850`），真实尝试次数是乘法关系。

`hub_set_address()`（4755）体现了一个架构选择：**xHCI 不用控制传输发 SET_ADDRESS**。

```c
	if (!hcd->driver->address_device && devnum <= 1)
		return -EINVAL;
	if (udev->state == USB_STATE_ADDRESS)
		return 0;
	if (udev->state != USB_STATE_DEFAULT)
		return -EINVAL;
	if (hcd->driver->address_device)
		retval = hcd->driver->address_device(hcd, udev, timeout_ms);
	else
		retval = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				USB_REQ_SET_ADDRESS, 0, devnum, 0,
				NULL, 0, timeout_ms);
	if (retval == 0) {
		update_devnum(udev, devnum);
		usb_set_device_state(udev, USB_STATE_ADDRESS);
		usb_ep0_reinit(udev);
	}
```

xHCI 用 `Address Device` 命令完成"分配地址 + 建立 slot 上下文"，只有 EHCI/UHCI/OHCI 才真正发 `SET_ADDRESS` 控制传输。

### 3.4 usb_new_device 与接口注册

拿到描述符后 `usb_new_device()`（2647）负责设备模型接入，devt 算法也在这里：

```c
	err = usb_enumerate_device(udev);	/* Read descriptors */
	udev->dev.devt = MKDEV(USB_DEVICE_MAJOR,
			(((udev->bus->busnum-1) * 128) + (udev->devnum-1)));
	device_enable_async_suspend(&udev->dev);
	set_usb_port_removable(udev);
	err = device_add(&udev->dev);
```

`usb_enumerate_device()`（2526）依次做四件事：`usb_get_configuration()`（`config.c:907`）读并解析全部配置；`usb_cache_string()` 读 iProduct/iManufacturer/iSerialNumber；`usb_enumerate_device_otg()`（2440）处理 OTG/HNP；`usb_detect_interface_quirks()`（2565）打怪癖标记。`device_add()` 之后由 `usb_bus_type` 匹配到 `usb_generic_driver`，调用链为 `usb_device_match()` → `usb_probe_device()`（driver.c:246）→ `usb_generic_driver_probe()`（generic.c:238）→ `usb_choose_configuration()`（generic.c:52）→ `usb_set_configuration()`（message.c:2056）。

`usb_choose_configuration()` 先让 device driver 自选，否则遍历配置、跳过 `usb_get_max_power() > udev->bus_mA` 的配置，音频设备特判 UAC3，取第一个合格项。`usb_set_configuration()` 是"把配置变成活对象"的枢纽：

```c
	mutex_lock(hcd->bandwidth_mutex);
	if (dev->actconfig && usb_disable_lpm(dev)) { ... }
	ret = usb_hcd_alloc_bandwidth(dev, cp, NULL, NULL);
	for (i = 0; i < nintf; ++i) {
		cp->interface[i] = intf = new_interfaces[i];
		intfc = cp->intf_cache[i];
		intf->altsetting = intfc->altsetting;
		intf->num_altsetting = intfc->num_altsetting;
		intf->authorized = !!HCD_INTF_AUTHORIZED(hcd);
		kref_get(&intfc->ref);
		intf->dev.bus = &usb_bus_type;
		intf->dev.type = &usb_if_device_type;
	}
```

随后对每个 `intf->dev` 调 `device_add()`，driver core 以 `usb_device_match()` 匹配 `usb_driver` 并进入 `usb_probe_interface()`（driver.c:318）：

```c
	if (usb_device_is_owned(udev))
		return error;
	if (udev->authorized == 0) { ... return error; }
	else if (intf->authorized == 0) { ... return error; }
	id = usb_match_dynamic_id(intf, driver);
	if (!id)
		id = usb_match_id(intf, driver->id_table);
	if (!id)
		return error;
	error = usb_autoresume_device(udev);
	...
	intf->condition = USB_INTERFACE_BINDING;
	error = driver->probe(intf, id);
```

两道授权关卡（设备级 `udev->authorized`、接口级 `intf->authorized`）是"lsusb 看得到却没有 /dev 节点"的常见原因。拔出时 `usb_disconnect()`（2315）先把状态设为 `NOTATTACHED`，使所有新 URB 立即失败，再 `usb_disable_device(udev, 0)` 注销接口（`message.c:1452`）、`usb_hcd_synchronize_unlinks()`、`device_del()`、释放设备号；复位路径 `usb_reset_and_verify_device()`（6178）会校验描述符是否变化，变化则强制解绑重枚举，`usb_reset_device()`（6357）是对外包装，负责 `pre_reset/post_reset/reset_resume`。

## 4. URB 生命周期

### 4.1 分配与提交

```c
struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
	struct urb *urb;

	urb = kmalloc_flex(*urb, iso_frame_desc, iso_packets, mem_flags);
	if (!urb)
		return NULL;
	usb_init_urb(urb);
	return urb;
}
```

本树用 `kmalloc_flex()`（新分配宏）而非旧的 `kzalloc(sizeof(*urb) + n*...)`；`usb_free_urb()` 仅 `kref_put()`（urb.c:93），最后一个引用才真正释放，transfer buffer 默认不随 URB 释放，除非置 `URB_FREE_BUFFER`。填充接口 `usb_fill_control_urb()`/`usb_fill_bulk_urb()`/`usb_fill_int_urb()`（usb.h:1697/1732/1748）只填公共字段、不做校验。`usb_submit_urb()`（urb.c:367）绝大部分是校验：

```c
	if (!urb || !urb->complete)
		return -EINVAL;
	if (urb->hcpriv) {
		WARN_ONCE(1, "URB %p submitted while active\n", urb);
		return -EBUSY;
	}
	dev = urb->dev;
	if ((!dev) || (dev->state < USB_STATE_UNAUTHENTICATED))
		return -ENODEV;
	ep = usb_pipe_endpoint(dev, urb->pipe);
	if (!ep)
		return -ENOENT;
	urb->ep = ep;
	urb->status = -EINPROGRESS;
	xfertype = usb_endpoint_type(&ep->desc);
	if (xfertype == USB_ENDPOINT_XFER_CONTROL) {
		struct usb_ctrlrequest *setup =
				(struct usb_ctrlrequest *) urb->setup_packet;
		if (!setup)
			return -ENOEXEC;
		is_out = !(setup->bRequestType & USB_DIR_IN) || !setup->wLength;
		if (le16_to_cpu(setup->wLength) != urb->transfer_buffer_length)
			return -EBADR;
	}
	return usb_hcd_submit_urb(urb, mem_flags);
```

两个常被误传的点：设备状态门槛是 `UNAUTHENTICATED` 而非 `CONFIGURED`，只有非控制端点才要求 `state >= USB_STATE_CONFIGURED`（432-434）；同步端点的 `interval` 会被"向下取 2 的幂"裁剪（582-583），填 3 最终变成 2。

### 4.2 HCD 入队与端点挂链

```c
int usb_hcd_submit_urb (struct urb *urb, gfp_t mem_flags)
{
	int status;
	struct usb_hcd *hcd = bus_to_hcd(urb->dev->bus);

	usb_get_urb(urb);
	atomic_inc(&urb->use_count);
	atomic_inc(&urb->dev->urbnum);
	usbmon_urb_submit(&hcd->self, urb);
	if (is_root_hub(urb->dev)) {
		status = rh_urb_enqueue(hcd, urb, mem_flags);
	} else {
		status = map_urb_for_dma(hcd, urb, mem_flags);
		if (likely(status == 0)) {
			status = hcd->driver->urb_enqueue(hcd, urb, mem_flags);
			if (unlikely(status))
				unmap_urb_for_dma(hcd, urb);
		}
	}
	...
}
```

根 hub 的 URB 不经过硬件，而由 `rh_urb_enqueue()`（hcd.c:811）模拟：控制请求走 `rh_call_control()`（449），状态请求由 `rh_timer_func()` 轮询。真实 HCD 会先 `usb_hcd_link_urb_to_ep()`（1126）把 URB 挂到 `ep->urb_list`，并逐项检查 `reject`、`ep->enabled`、`dev->can_submit` 与 `HCD_RH_RUNNING`；任何一项不过就返回 `-EPERM/-ENOENT/-EHOSTUNREACH/-ESHUTDOWN`，这是所有入口错误码的来源。

### 4.3 完成：giveback

`usb_hcd_giveback_urb()`（1731）决定立即完成还是排队：

```c
	if (likely(!urb->unlinked))
		urb->unlinked = status;
	if (!hcd_giveback_urb_in_bh(hcd) && !is_root_hub(urb->dev)) {
		__usb_hcd_giveback_urb(urb);
		return;
	}
	if (usb_pipeisoc(urb->pipe) || usb_pipeint(urb->pipe))
		bh = &hcd->high_prio_bh;
	else
		bh = &hcd->low_prio_bh;
```

真正的交还点是 `__usb_hcd_giveback_urb()`（1630），顺序不能乱：

```c
static void __usb_hcd_giveback_urb(struct urb *urb)
{
	struct usb_hcd *hcd = bus_to_hcd(urb->dev->bus);
	int status = urb->unlinked;

	urb->hcpriv = NULL;
	if (unlikely((urb->transfer_flags & URB_SHORT_NOT_OK) &&
	    urb->actual_length < urb->transfer_buffer_length && !status))
		status = -EREMOTEIO;
	unmap_urb_for_dma(hcd, urb);
	usbmon_urb_complete(&hcd->self, urb, status);
	urb->status = status;
	urb->complete(urb);
	atomic_dec(&urb->use_count);
	if (unlikely(atomic_read(&urb->reject)))
		wake_up(&usb_kill_urb_queue);
	usb_put_urb(urb);
}
```

三个要点：短包仅在 `URB_SHORT_NOT_OK` 时为 `-EREMOTEIO`，否则算成功；`complete` 里可以立即释放或重提交（`hcpriv` 已清、引用仍在）；回调上下文默认是 **BH workqueue**（`system_bh_wq`/`system_bh_highpri_wq`），不是硬中断。旧文档笼统说"中断上下文"，会误导驱动作者在回调里误用睡眠函数；未设 `HCD_BH` 时，非根 hub URB 可在调用者上下文直接完成。

### 4.4 取消、kill、poison 与 DMA

```text
usb_unlink_urb → usb_hcd_unlink_urb(-ECONNRESET) → hcd->driver->urb_dequeue()
                 异步，成功返回 -EINPROGRESS；回调看到 -ECONNRESET
usb_kill_urb   → atomic_inc(&urb->reject) → usb_hcd_unlink_urb(-ENOENT)
               → 等待 use_count 归零，保证 complete 已跑完
usb_poison_urb → 同 kill，但 reject 保持（除非 unpoison），永久不可再提交
```

`usb_kill_urb()`（703）会睡眠，不能用于中断/BH；`usb_unlink_urb()`（664）不等待；anchor 版本 `usb_kill_anchored_urbs()`（812）、`usb_wait_anchor_empty_timeout()`（941）是 disconnect 的标准收尾。顺序陷阱：必须"先 kill、后释放 context"，反过来就是 UAF——kill 的语义价值正在于"complete 已结束"。

DMA 映射由 `map_urb_for_dma()` 完成，默认走 `usb_hcd_map_urb_for_dma()`：控制传输的 setup packet 若在栈上会被拒绝（`object_is_on_stack()` → `-EAGAIN`）；transfer buffer 用 `dma_map_single()` 或 `dma_map_sg()`；`URB_NO_TRANSFER_DMA_MAP` 表示驱动已自行映射；`uses_pio_for_control` 的控制器不做映射。驱动分配 DMA 缓冲应使用 `usb_alloc_coherent()`（usb.c:1006），底层 `hcd_buffer_alloc()`（hcd.h:515）从 `hcd->pool[4]` 取池；`kmalloc()` 也可，但不要把缓冲区嵌在可能共享 cache 行的结构体中间——`usb_fill_control_urb()` 的注释（usb.h:1688-1694）专门警告了这种 cache 不一致。

## 5. HCD 层与 xHCI

### 5.1 注册与根 hub

```text
usb_create_hcd(driver, dev, bus_name)      hcd.c:2659
usb_add_hcd(hcd, irqnum, irqflags)         hcd.c:2802
  → hcd_buffer_create()                    hcd.c:2870
  → usb_register_bus(&hcd->self)           hcd.c:2876
  → rhdev = usb_alloc_dev(NULL, &hcd->self, 0)
  → 按 hcd->speed 设置 rhdev->speed         hcd.c:2894
  → hcd->driver->reset(hcd)                hcd.c:2934
  → usb_hcd_request_irqs()                 hcd.c:2962
  → register_root_hub()                    hcd.c:951
```

`register_root_hub()` 把根 hub 设备号固定为 1，读设备描述符后 `usb_new_device()`。根 hub 没有真实设备，`urb_enqueue` 由 `rh_urb_enqueue()` 模拟，`hub_control` 回调把 hub 类请求翻译成端口操作。

### 5.2 xHCI 的硬件模型

```text
               ┌──────────────┐
  DCBAA ──────►│ Device Slot  │──► out_ctx: Slot Ctx + Endpoint Ctx[1..31]
               └──────────────┘                    │ deq
               ┌──────────────┐             ┌──────▼───────┐
   CRCR ──────►│ Command Ring │             │ Transfer Ring│ 每端点一条
               └──────┬───────┘             └──────┬───────┘
                      │ TRB                        │ TRB
               ┌──────▼───────┐             ┌──────▼───────┐
   ERST ──────►│ Event Ring   │◄────────────│  xHC 硬件     │
               └──────────────┘             └──────┬───────┘
               ┌──────────────┐                    │
  Doorbell ───►│ DB[slot]     │────────────────────┘
               └──────────────┘
```

命令环是软件生产者、硬件消费者，承载 `Enable Slot`、`Address Device`、`Configure Endpoint`、`Set TR Dequeue`；事件环每个 interrupter 一条，硬件生产者、软件消费者；传输环每个端点一条，URB 拆成 TD、TD 由若干 TRB 组成；doorbell 用于软件写完 TRB 后通知硬件；上下文里 slot context 描述设备（route、speed、hub 标志），endpoint context 描述端点（类型、max packet、dequeue、interval）。

```c
struct xhci_ring {
	struct xhci_segment	*first_seg;
	struct xhci_segment	*last_seg;
	union  xhci_trb		*enqueue;
	struct xhci_segment	*enq_seg;
	union  xhci_trb		*dequeue;
	struct xhci_segment	*deq_seg;
	struct list_head	td_list;
	u32			cycle_state;
	unsigned int		num_segs;
	enum xhci_ring_type	type;
};

struct xhci_slot_ctx {
	__le32	dev_info;	/* route string + speed + hub 标志 */
	__le32	dev_info2;
	__le32	tt_info;
	__le32	dev_state;	/* slot state + device address */
};
```

`cycle_state` 是环形队列的所有权协议：TRB 的 cycle bit 与 ring 的 `cycle_state` 一致才表示"本次有效"，绕回起点时翻转，读 xHCI 代码时几乎所有的 `TRB_CYCLE` 判断都与它有关。

### 5.3 初始化与运行

`xhci_gen_setup()`（5426）缓存 capability 参数、分配 `xhci->devs[]`、命令环、事件环与 scratchpad，之后 `reset = xhci_reset`、`start = xhci_run`。`xhci_init()`（533）的顺序值得记：设置 max slots → 初始化命令环 → 写 CRCR → 写 DCBAAP → 定位 doorbell array → 初始化 primary 事件环 → `xhci_add_interrupter()`。`xhci_run()`（626）是最后一步：

```c
	hcd->uses_new_polling = 1;
	if (hcd->msi_enabled)
		ir->ip_autoclear = true;
	if (!usb_hcd_is_primary_hcd(hcd))
		return xhci_run_finished(xhci);
	...
	if (xhci_has_one_roothub(xhci))
		return xhci_run_finished(xhci);
	set_bit(HCD_FLAG_DEFER_RH_REGISTER, &hcd->flags);
	return 0;
```

`xhci_run_finished()`（578）打开 `CMD_EIE`、使能 primary interrupter、`xhci_start()`，并把 `cmd_ring_state` 置 RUNNING。USB3 控制器通常有 primary（USB3）与 shared（USB2）两个 hcd，共享一套寄存器，因此根 hub 注册要延后到两者就绪（`HCD_FLAG_DEFER_RH_REGISTER`）——这就是"xHCI 为什么有 usb1/usb2 两个 root hub"的源码依据。

### 5.4 URB 入队：TRB 生成

`xhci_urb_enqueue()`（1619）先算 TD 数：等时端点按包数，bulk OUT 在置 `URB_ZERO_PACKET` 且长度是 maxpacket 整数倍时为 2（多发一个零长包），其余为 1：

```c
	if (usb_endpoint_xfer_isoc(&urb->ep->desc))
		num_tds = urb->number_of_packets;
	else if (usb_endpoint_is_bulk_out(&urb->ep->desc) &&
	    urb->transfer_buffer_length > 0 &&
	    urb->transfer_flags & URB_ZERO_PACKET &&
	    !(urb->transfer_buffer_length % usb_endpoint_maxp(&urb->ep->desc)))
		num_tds = 2;
	else
		num_tds = 1;
```

随后按端点类型分发到 `xhci_queue_ctrl_tx/bulk_tx/intr_tx/isoc_tx_prepare`。所有路径最终用 `queue_trb()` 写 TRB，并在最后 `giveback_first_trb()` 敲 doorbell：

```c
static void giveback_first_trb(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index, unsigned int stream_id, int start_cycle,
		struct xhci_generic_trb *start_trb)
{
	wmb();
	if (start_cycle)
		start_trb->field[3] |= cpu_to_le32(start_cycle);
	else
		start_trb->field[3] &= cpu_to_le32(~TRB_CYCLE);
	xhci_ring_ep_doorbell(xhci, slot_id, ep_index, stream_id);
}
```

`queue_trb()`（3217）自身也有写屏障：field[0..2] 写完后 `wmb()`，最后才写含 cycle bit 与 TRB 类型的 field[3]。先写 field[3] 会让硬件看到半成品。`xhci_ring_ep_doorbell()`（549）在端点正在停止、Set Dequeue 未完成、已 halt、正在清 TT buffer 时故意不敲门铃，这是 dequeue 竞态防护的核心。

### 5.5 中断与事件处理

```c
irqreturn_t xhci_irq(struct usb_hcd *hcd)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	...
	status = readl(&xhci->op_regs->status);
	if (status == ~(u32)0) {
		xhci_hc_died(xhci);
		goto out;
	}
	if (!(status & STS_EINT)) {
		ret = IRQ_NONE;
		goto out;
	}
	if (status & STS_HCE) { xhci_halt(xhci); goto out; }
	if (status & STS_FATAL) { xhci_halt(xhci); goto out; }
	/* USBSTS bits are write 1 to clear */
	writel(STS_EINT, &xhci->op_regs->status);
	xhci_handle_events(xhci, xhci->interrupters[0], false);
out:
	spin_unlock(&xhci->lock);
	return ret;
}
```

事件循环 `xhci_handle_events()`（3065）逐条处理事件 TRB，每半段更新一次 ERDP，并在处理等时事件时逐步缩小 `isoc_bei_interval` 以减少中断：

```c
	xhci_clear_interrupt_pending(ir);
	while (unhandled_event_trb(ir->event_ring)) {
		if (!skip_events)
			err = xhci_handle_event_trb(xhci, ir, ir->event_ring->dequeue);
		if (event_loop++ > TRBS_PER_SEGMENT / 2) {
			xhci_update_erst_dequeue(xhci, ir, false);
			if (ir->isoc_bei_interval > AVOID_BEI_INTERVAL_MIN)
				ir->isoc_bei_interval = ir->isoc_bei_interval / 2;
			event_loop = 0;
		}
		inc_deq(xhci, ir->event_ring);
		if (err)
			break;
	}
	xhci_update_erst_dequeue(xhci, ir, true);
```

`xhci_update_erst_dequeue()`（3017）写 ERDP 时遵守"只前进不后退"，并可清 EHB；`ip_autoclear` 为真（MSI 常见）时无需手动清 IMAN.IP。命令完成由 `handle_cmd_completion()`（1795）按 `TRB_ENABLE_SLOT/DISABLE_SLOT/CONFIG_EP/ADDR_DEV/STOP_RING` 等类型分派，`struct xhci_command` 的 `completion` 负责唤醒同步等待者，为 NULL 则表示完成后自行释放。

### 5.6 EHCI 与 UHCI 对照

EHCI（USB2 高速）用周期性帧列表 + 异步 QH 链：

```text
ehci_irq (ehci-hcd.c:712)
  → 读 USBSTS 并清中断；STS_HCE/STS_FATAL 视为控制器致命错误
  → STS_INT/STS_ERR → bh=1
  → STS_IAA → 完成 unlink（异步推进）
  → STS_PCD → usb_hcd_poll_rh_status()
  → ehci_work() (386) → scan_async / scan_intr / scan_isoc
                     → qh_completions() (ehci-q.c:286) → usb_hcd_giveback_urb()
```

UHCI（USB1.1）用 1024 项帧列表 + 骨架 QH，TD 是 16 字节对齐的四个 32 位寄存器（link/status/token/buffer）加软件字段。`uhci_urb_enqueue()`（uhci-q.c:1408）按端点类型调用 `uhci_submit_control/bulk/interrupt/isochronous`，把 URB 挂到端点 QH；`uhci_scan_schedule()`（1741）扫描调度，`uhci_giveback_urb()`（1516）归还 URB。UHCI 独有的 FSBR（全速带宽回收）把部分全速事务提到帧起始之前，弥补 USB1.1 带宽不足；OHCI 介于两者之间，用 ED+TD 与 HCCA 周期性调度，可与 UHCI 对照阅读。

## 6. Hub 与端口状态机

### 6.1 hub 对象与状态位

`struct usb_hub`（`hub.h:20`）的要点：`urb`/`buffer`/`status` 构成状态轮询通道（8 字节变化位图）；`event_bits` 是 hub 级或端口级事件位图（bit0 为 hub 自身）；`change_bits` 表示逻辑连接变化；`removed_bits` 标记"设备已移除"以抑制重复枚举；`wakeup_bits`/`power_bits`/`child_usage_bits` 分别服务远程唤醒、端口供电与 runtime PM；`descriptor`/`tt`/`ports` 是 hub 类描述符、Transaction Translator 与端口数组。

| 状态位 | 值 | 含义 | 变化位（`_C_`） |
|--------|----|------|----------------|
| `CONNECTION` | 0x0001 | 有设备连接 | `C_CONNECTION` 0x0001 |
| `ENABLE` | 0x0002 | 端口已使能 | `C_ENABLE` 0x0002 |
| `SUSPEND` | 0x0004 | 挂起 | `C_SUSPEND` 0x0004 |
| `OVERCURRENT` | 0x0008 | 过流 | `C_OVERCURRENT` 0x0008 |
| `RESET` | 0x0010 | 复位进行中 | `C_RESET` 0x0010 |
| `POWER` | 0x0100 | 端口供电 | — |
| `LOW_SPEED` / `HIGH_SPEED` | 0x0200 / 0x0400 | 低速 / 高速设备 | — |

状态与变化是**两个独立的 16 位字段**（`ch11.h:123-178`），`GET_STATUS` 返回 4 字节：低 2 字节状态、高 2 字节变化。

### 6.2 端口事件处理

`port_event()`（5754）负责"清变化位 + 决定是否重新连接"，其中"EMI 导致 hub 关闭端口"是真实故障，这段代码就是为它准备的：

```c
	if (portchange & USB_PORT_STAT_C_CONNECTION) {
		usb_clear_port_feature(hdev, port1, USB_PORT_FEAT_C_CONNECTION);
		connect_change = 1;
	}
	if (portchange & USB_PORT_STAT_C_ENABLE) {
		usb_clear_port_feature(hdev, port1, USB_PORT_FEAT_C_ENABLE);
		if (!(portstatus & USB_PORT_STAT_ENABLE)
		    && !connect_change && udev) {
			dev_err(&port_dev->dev, "disabled by hub (EMI?), re-enabling...\n");
			connect_change = 1;
		}
	}
```

过流处理会先清位、`msleep(100)` 冷却、重新上电、再读状态确认（5795-5809）。随后 `hub_handle_remote_wakeup()` 判定远程唤醒；USB3 的 warm reset 恢复逻辑在 5849-5876：链路处于 SS.Inactive/Compliance 时先等最多 5×20 ms 让断开检测生效，再决定"只复位端口"还是"整设备复位"——这解释了 USB3 设备偶发"要插拔两次"。`hub_power_on()`（962）按 `bPwrOn2PwrGood` 延时，`hub_port_debounce_be_stable()`（5427）要求连接连续稳定才认为有效。

### 6.3 电源预算与挂起

`hub_configure()`（1449）从 hub 描述符算 `mA_per_port`，`hub_port_connect()` 对"总线供电 hub 串接"直接拒绝：

```c
		if (udev->descriptor.bDeviceClass == USB_CLASS_HUB
				&& udev->bus_mA <= unit_load) {
			...
			if ((devstat & (1 << USB_DEVICE_SELF_POWERED)) == 0) {
				dev_err(&udev->dev, "can't connect bus-powered hub "
					"to this port\n");
				status = -ENOTCONN;	/* Don't retry */
```

而 `usb_set_configuration()` 对配置电流超预算只告警不拒绝（message.c:2108-2110），这是兼容性取舍。挂起路径：普通设备用 `usb_port_suspend()/usb_port_resume()`，根 hub 没有上游端口，走 `hcd_bus_suspend()/hcd_bus_resume()`（hcd.c:2258/2311）。hub 自身默认 `autosuspend_delay = 0`（hub.c:1921-1924）以加快总线挂起，与普通设备相反；`usb_enable_autosuspend()` 系列（usb.h:809-850）把 USB PM 接到 runtime PM，`supports_autosuspend` 为假的驱动会让接口保持 active。

## 7. USB 存储（usb-storage）

### 7.1 分层与传输选择

```text
SCSI midlayer（scsi_add_host / queuecommand）
   ↑ scsi_cmnd
usb-storage: scsiglue.c（host template）→ usb.c（控制线程 us_data）
   ↓ proto_handler
protocol.c: transparent / 8020i / QIC-157 / UFI …
   ↓ transport
transport.c: Bulk-Only (BOT) / Control-Bulk (CB) / CBI
   ↓ urb
usbcore
```

`us->transport` 由接口 `bInterfaceProtocol` 决定（`get_transport()`，usb.c:748）：`USB_PR_CB/CBI` 用 `usb_stor_CB_transport`，`USB_PR_BULK` 用 `usb_stor_Bulk_transport`；绝大多数 U 盘是 `USB_PR_BULK` + `USB_SC_SCSI`（transparent）。`usb_stor_control_thread()`（usb.c:380）等待 `us->cmnd_ready` 后调用 `us->proto_handler(srb, us)`；线程化是因为 BOT 事务可能睡眠（控制/批量传输、复位），不能放在 SCSI 软中断上下文。

### 7.2 BOT 协议

```text
Host                              Device
 │  CBW (31 bytes, Bulk OUT)  ───►  │
 │  Data OUT / IN (可选)       ◄──► │
 │  CSW (13 bytes, Bulk IN)   ◄───  │
```

CBW 构造（transport.c:1128-1141）中，`Tag` 自增用于匹配 CSW，`Signature` 是 "USBC" 魔数：

```c
	bcb->Signature = cpu_to_le32(US_BULK_CB_SIGN);
	bcb->DataTransferLength = cpu_to_le32(transfer_length);
	bcb->Flags = srb->sc_data_direction == DMA_FROM_DEVICE ?
		US_BULK_FLAG_IN : US_BULK_FLAG_OUT;
	bcb->Tag = ++us->tag;
	bcb->Lun = srb->device->lun;
	if (us->fflags & US_FL_SCM_MULT_TARG)
		bcb->Lun |= srb->device->id << 4;
	bcb->Length = srb->cmd_len;
	memset(bcb->CDB, 0, sizeof(bcb->CDB));
	memcpy(bcb->CDB, srb->cmnd, bcb->Length);
```

接收 CSW 后校验签名与 Tag，再按 `bCSWStatus` 决定成功、失败或相位错误（Phase Error 需要 reset recovery）。异常处理是 usb-storage 最复杂的部分：

| 情况 | 处理 |
|------|------|
| Bulk IN 短包正好 13 字节且是 CSW | 判定设备跳过 Data 阶段，`scsi_set_resid(srb, transfer_length)`（1190-1223） |
| 设备返回数据多于请求（`USB_STOR_XFER_LONG`） | 伪造 sense：Illegal Request / Invalid Field in CDB |
| Stall/EPIPE | `usb_stor_clear_halt()`（227）清 halt 后重试 |
| 超时 | 置 `US_FLIDX_TIMED_OUT`，控制线程放弃命令，`DID_ABORT`（usb.c:408-412） |
| 需要复位 | `usb_stor_reset_common()`（1359）：Bulk-Only Reset + 清两个 bulk halt |

`usb_stor_invoke_transport()`（604）在外层做重试与 sense 翻译，包含"CSW 失败后自动补发 REQUEST SENSE"，因为很多设备要求主机主动取 sense 才给出真实错误。

### 7.3 unusual_devs.h 与 UAS

`unusual_devs.h` 是按 VID/PID 列怪癖的大表（本树 324 处 `UNUSUAL_DEV/UNUSUAL_DEV_SKIP`，含子文件），条目参数为 VID、PID、bcdDevice 上下界、厂商名、产品名、子类覆盖、协议覆盖、初始化函数、flags；常见 flags 有 `US_FL_FIX_INQUIRY`、`US_FL_SINGLE_LUN`、`US_FL_IGNORE_RESIDUE`、`US_FL_NO_READ_CAPACITY_16`、`US_FL_BULK32`、`US_FL_GO_SLOW`。源码顶部（44-54）明确写着**内核内模式切换已废弃**，新设备应使用用户态 usb_modeswitch 数据库——这与老教程"加一行 UNUSUAL_DEV 切模式"的说法不同。另外 `storage_probe()`（1194）在 UAS 可用且设备支持时直接 `-ENXIO` 让 `uas` 接管（1202-1206）；`uas.c` 使用 USB3 streams 与多命令并发，性能远高于 BOT，`unusual_uas.h` 是"号称支持 UAS 但有问题"的黑名单。

## 8. Gadget 侧概览

### 8.1 与主机侧的对应关系

| 主机侧 | gadget 侧 |
|--------|-----------|
| `struct usb_device` | `struct usb_gadget` |
| `struct usb_host_endpoint` | `struct usb_ep` |
| `struct urb` | `struct usb_request` |
| `usb_submit_urb()` | `usb_ep_queue()` |
| HCD（xhci/ehci） | UDC（dwc3/dummy_hcd/…） |
| class 驱动 | function 驱动（f_acm/f_mass_storage） |

```c
int usb_ep_queue(struct usb_ep *ep,
			       struct usb_request *req, gfp_t gfp_flags)
{
	int ret = 0;

	if (!ep->enabled && ep->address) {
		pr_debug("USB gadget: queue request to disabled ep 0x%x (%s)\n",
				 ep->address, ep->name);
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = ep->ops->queue(ep, req, gfp_flags);
out:
	trace_usb_ep_queue(ep, req, ret);
	return ret;
}
```

完成语义与 URB 一致：**提交成功则 `req->complete()` 恰好调用一次**，之后 request 可立即复用或释放（core.c:287-291）。

### 8.2 UDC 注册

`usb_add_gadget()`（core.c:1410）创建 `struct usb_udc`，`device_initialize()` 后设 `udc_class`、名字取父设备 kobject 名，挂入 `udc_list`，再 `device_add(&udc->dev)`，最后 `usb_gadget_set_state(gadget, USB_STATE_NOTATTACHED)`，结果是在 `/sys/class/udc/<controller>` 出现一个可绑定实例。`usb_gadget_register_driver_owner()`（1740）把 `usb_gadget_driver` 注册到 `gadget_bus_type`，用 `PROBE_FORCE_SYNCHRONOUS` 同步 probe，再在 `udc_list` 中寻找空闲 UDC 绑定——这解释了"驱动已注册但未绑定"的中间状态。

### 8.3 composite、configfs、function

- **composite**（`composite.c`）：把多个 function 组合成一个设备，统一处理 Chapter 9 请求（`composite_setup()`，1754）、bind/unbind、字符串与配置描述符拼装，`usb_add_config()`（1105）添加配置。
- **configfs**（`configfs.c`）：用户态在 `/sys/kernel/config/usb_gadget/<name>/` 下建目录，最后写 `UDC` 生效；`configfs_composite_bind()`（1685）在绑定 UDC 时才真正实例化 function。
- **function**（`function/`）：`f_acm`、`f_ecm/f_ncm/f_rndis/f_subset`、`f_mass_storage`、`f_fs`（FunctionFS，用户态协议）、`f_hid`、`f_midi`、`f_uac2` 等。
- **legacy**（`legacy/`）：`g_ether`、`g_mass_storage`、`g_serial` 等硬编码模块，仍可用但不如 configfs 灵活。

### 8.4 Type-C

`drivers/usb/typec/class.c` 是连接器类：`typec_register_port()`、`typec_set_data_role()`（2196）、`typec_set_pwr_role()`（2238）、`typec_register_partner()`、altmode 注册。它只描述"连接器状态与角色"，不传输数据；`typec_attach()` 在 `usb_new_device()` 中被调用（hub.c:2730），把 USB 设备与连接器关联，供用户态读取 `/sys/class/typec/`。

## 9. sysfs / debugfs 观测点与常见问题

| 路径 | 内容 |
|------|------|
| `/sys/bus/usb/devices/` | 设备与接口，命名 `usbN` / `N-M[.P]` / `N-M:cfg.if` |
| `.../idVendor`、`idProduct`、`bcdDevice`、`speed`、`bMaxPower` | 设备属性 |
| `.../authorized`、`.../authorized_default` | 授权开关 |
| `.../power/control`、`autosuspend_delay_ms`、`wakeup` | runtime PM |
| `.../remove` | 手动注销设备（调热插拔） |
| `/sys/class/udc/`、`/sys/kernel/config/usb_gadget/` | gadget 与 configfs |
| `/sys/kernel/debug/usb/devices` | 文本拓扑（`usbfs_devices_fops`，usb.c:1199） |
| `/sys/kernel/debug/usb/usbmon/` | usbmon 抓包（`CONFIG_USB_MON`） |

`usb_new_device()` 的 devt 算法（hub.c:2680）为 `MKDEV(USB_DEVICE_MAJOR, ((busnum-1)*128) + (devnum-1))`，即总线号从 1 起、每总线 128 个设备号，`/dev/bus/usb/001/002` 就是 bus 1、dev 2。

| 现象 | 可能原因 | 排查点 |
|------|---------|--------|
| `device descriptor read/64, error -110` | 供电不足、线缆、固件未就绪 | 换口换线；看是否打印 `new high-speed USB device` |
| `device not accepting address N, error -71` | `SET_ADDRESS` 失败，信号问题 | `usbcore.old_scheme_first=1`；USB3 走 `address_device` |
| `unable to enumerate USB device` | 复位后连接未恢复 | 端口供电、hub 兼容性、`HUB_RESET_TIMEOUT` 耗尽 |
| `can't set config #1, error -12` | 带宽或内存不足 | `usb_hcd_alloc_bandwidth()` 失败；isoc 端点与拓扑 |
| `URB ... submitted while active` | 同一 URB 未完成就重提交 | 驱动 bug，查 `complete` 回调 |
| `usb_submit_urb` 返回 `-EPERM` | `usb_kill_urb()` 正在跑（reject 置位） | 检查并发的 kill/submit |
| `-EMSGSIZE` / `-ENOSPC` | 端点 maxpacket 为 0 或等时包超限 | 描述符解析、`usb_endpoint_maxp()` |
| 设备出现但无驱动 | `authorized=0` 或 id_table 不匹配 | `intf->condition`、`/sys/.../driver` 软链 |
| 挂起后无法唤醒 | `wakeup` 未开或 `needs_remote_wakeup` | `/sys/.../power/wakeup`、`USB_DEVICE_REMOTE_WAKEUP` |

调试参数：`usbcore.autosuspend=-1`（禁用自动挂起）、`usbcore.old_scheme_first=1`、`usbcore.use_both_schemes=1`、`usbcore.initial_descriptor_timeout=`、`usb-storage.delay_use=` 在源码中都有对应变量（`hub.c` 顶部、`usb.c`）。用 `dynamic_debug` 打开 `drivers/usb/core/*` 的 `dev_dbg` 是定位枚举问题的首选。

## 10. 配置项

本树 `.config` 实际内容（grep 结果）：

```text
# .config:2680
CONFIG_USB_OHCI_LITTLE_ENDIAN=y
# .config:2681
# CONFIG_USB_SUPPORT is not set
# .config:3136
# CONFIG_USB4 is not set
```

`CONFIG_USB`、`CONFIG_USB_XHCI_HCD`、`CONFIG_USB_EHCI_HCD`、`CONFIG_USB_STORAGE`、`CONFIG_TYPEC`、`CONFIG_USB_GADGET` 均未出现在 `.config`，因为它们依赖 `USB_SUPPORT`，整体未启用。

| 配置项 | 作用 | 本树状态 |
|--------|------|---------|
| `CONFIG_USB_SUPPORT` | USB 总开关（`drivers/usb/Kconfig:29`） | 未启用 |
| `CONFIG_USB_XHCI_HCD`、`EHCI_HCD`、`OHCI_HCD`、`UHCI_HCD` | 各代主机控制器 | 未启用 |
| `CONFIG_USB_STORAGE`、`CONFIG_USB_UAS` | 存储类驱动 | 未启用 |
| `CONFIG_USB_GADGET`、`CONFIG_TYPEC` | gadget 框架与 Type-C | 未启用 |
| `CONFIG_USB_MON`、`CONFIG_USB_OTG`、`CONFIG_USB4` | usbmon、OTG、USB4 隧道 | 未启用 |

> 源码存在，当前配置未启用。若要编译验证，需打开 `CONFIG_USB_SUPPORT=y` 及对应 HCD（如 `CONFIG_USB_XHCI_HCD=y`）后 `make olddefconfig`；不要据此认为源码缺失。`CONFIG_USB_FEW_INIT_RETRIES` 开启后把复位/取描述符重试减半（hub.c:2893-2907），用于加快启动、牺牲兼容性。

## 11. 关键文件清单

| 文件 | 行数 | 职责 |
|------|------|------|
| `include/linux/usb.h` | 2117 | 主机侧核心结构、内联填充函数、API |
| `include/linux/usb/hcd.h` | — | `usb_hcd`、`hc_driver`、HCD 公共 API |
| `drivers/usb/core/usb.c` | 1294 | 设备分配/引用、DMA 缓冲、初始化、debugfs |
| `drivers/usb/core/message.c` | 2521 | 同步传输 API、描述符读取、SetConfiguration |
| `drivers/usb/core/urb.c` | 1021 | URB 分配/提交/取消/anchor |
| `drivers/usb/core/hcd.c` | 3194 | HCD 注册、根 hub、排队/giveback、DMA、带宽 |
| `drivers/usb/core/hub.c` | 6575 | 枚举、端口状态机、hub 驱动、PM、复位 |
| `drivers/usb/core/driver.c` | 2081 | 总线匹配、probe/unbind、动态 id |
| `drivers/usb/core/generic.c` | — | 通用 device driver、选择配置 |
| `drivers/usb/core/sysfs.c`、`devices.c` | — | sysfs 属性、debugfs 拓扑 |
| `drivers/usb/host/xhci.c` | 5729 | xHCI 生命周期、urb enqueue/dequeue、上下文 |
| `drivers/usb/host/xhci-ring.c` | 4472 | TRB/环/事件/命令完成 |
| `drivers/usb/host/xhci-mem.c` | — | 环与上下文内存分配 |
| `drivers/usb/host/ehci-hcd.c` | 1401 | EHCI 主流程、中断、urb enqueue |
| `drivers/usb/host/ehci-q.c` | — | QH/TD、`qh_completions()` |
| `drivers/usb/host/uhci-hcd.c` | 935 | UHCI 初始化、中断、帧列表 |
| `drivers/usb/host/uhci-q.c` | — | UHCI QH/TD 调度、giveback |
| `drivers/usb/storage/usb.c` | 1258 | probe、控制线程、传输选择 |
| `drivers/usb/storage/transport.c` | — | BOT/CB/CBI 实现、复位恢复 |
| `drivers/usb/storage/protocol.c` | — | SCSI 命令封装、xfer buffer 访问 |
| `drivers/usb/storage/unusual_devs.h` | — | 设备怪癖表 |
| `drivers/usb/gadget/udc/core.c` | 1982 | gadget/UDC 框架、ep queue、绑定 |
| `drivers/usb/gadget/composite.c`、`configfs.c` | — | 复合设备、用户态配置 |
| `drivers/usb/typec/class.c` | — | Type-C 类与角色 |

## 12. 延伸阅读

本树 `Documentation/` 中真实存在的文档：`Documentation/driver-api/usb/URB.rst`、`anchors.rst`、`dma.rst` 讲 URB 语义、anchor 正确用法与 DMA 缓冲；`callbacks.rst`、`power-management.rst`、`hotplug.rst`、`bulk-streams.rst` 讲回调上下文、runtime PM、热插拔与 streams；`Documentation/usb/gadget_configfs.rst`、`Documentation/usb/ehci.rst`、`Documentation/usb/dwc3.rst` 讲 gadget 与控制器细节。

笔记库内相关章节：

- [总线与设备类框架：I2C / SPI / GPIO / pinctrl / regmap / USB / MMC / UFS](../bus/01-bus-framework.md) 第 9 节从总线框架角度横切 USB，可与本篇互补。
- [kobject 对象模型：引用计数、uevent 与 sysfs 映射](../driver-model/00-kobject.md) 理解 `struct device`、`kref`、`dev_set_name()` 的底层。
- [driver core：device/driver/bus 与 probe](../driver-model/01-driver-model.md) 理解 `need_parent_lock`、`device_add()`、`driver_register()` 的通用流程。
- [SCSI midlayer、UFS 与 MMC/eMMC](../../04-storage/scsi-ufs/01-scsi-ufs-emmc.md) 说明 usb-storage 只是 SCSI 的一个 host，先读 midlayer 再看第 7 节更顺。

外部资料按可信度排序：USB 2.0/3.2 与 xHCI 规范原文；`linux-usb@vger.kernel.org` 邮件列表与 `Documentation/` 更新；本树各文件头部注释与 Kconfig 注释——它们通常比二手文章准确。
