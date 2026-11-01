# QCA WCN7850 蓝牙固件（dada）

2 个文件供内核 `CONFIG_EXTRA_FIRMWARE` 编进 `.rodata` 使用。

## 为什么必须编进内核

和同目录的 `qcom/`（GPU）是**同一个时序坑**，只是发生在蓝牙这条路上。

`hci_qca` 驱动全部 built-in，走 initcall 探测，实测时间戳（当前 host 轨内核）：

```
[0.080441] hci_uart: Registered HCI driver
[0.080453] hci_qca: Registered HCI driver
[0.261067] hci_qca: setting up wcn7850
[0.319708] sda: sda1 sda2 ... sda36        ← 分区表刚枚举出来
[0.330907] QCA Downloading qca/gngbtfw20.mbn
[0.330959] Direct firmware load for qca/gngbtfw20.mbn failed with error -2
[0.657703] EXT4-fs (sda36): mounted filesystem   ← rootfs 到这才挂上
```

固件请求比 rootfs 挂载**早 327ms**，此时 `/lib/firmware` 还不可达，`request_firmware()`
必然 `-ENOENT`。

## 为什么这个 -ENOENT 特别致命（静默降级，不重试）

多数驱动 -ENOENT 会走 deferred probe 或报错重试，但 `hci_qca.c` 把它当成
「该平台本来就没有这个补丁」：

```c
/* linux/drivers/bluetooth/hci_qca.c:2089 */
if (ret == -ENOENT) {
    set_bit(QCA_ROM_FW, &qca->flags);
    ret = 0;              /* ← 探测"成功" */
}
```

于是 probe 返回 0，**不会再有 deferred probe 回来补下固件**。控制器永久停留在
ROM 固件状态（无 rampatch、无 NVM），实测后果：

- BD_ADDR 读成 `00:00:00:00:5A:AD`（ROM 态的垃圾值），而非 DT 里
  `local-bd-address` 指定的 `74:38:22:97:79:E5`
- NetworkManager 因而不创建蓝牙设备（`nmcli device status` 里 BT 设备数为 0）
- gnome-control-center 的 `container_bluetooth` 保持 `visible=False`，
  设置里**完全没有蓝牙**
- 但 BlueZ 仍能枚举 hci0，所以 `bluetoothctl` 看起来一切正常 —— 这正是
  「命令行能用、图形界面没有」的由来

内嵌后 `request_firmware()` 直接从 `.rodata` 取，时序窗口彻底消失。

## 用法（内核树内）

```
CONFIG_EXTRA_FIRMWARE="... qca/gngbtnv20.bin qca/gngbtfw20.mbn"
CONFIG_EXTRA_FIRMWARE_DIR="<此仓库 clone>/firmware"
```

构建脚本（`work/kernel-source/scripts/build-dada-debian-systemd-usb{host,debug}.sh`）
已通过 `ALL_FW="$GPU_FW $BT_FW"` 自动追加，无需手改。

## 名称必须与 DTS 逐字相等

驱动自己拼 `"qca/"` 前缀，所以 DT 里**不带**前缀：

```dts
/* work/patches/dada-consolidated/sm8750-xiaomi-dada-mainline.dts:2670 */
firmware-name = "gngbtnv20.bin", "gngbtfw20.mbn";
```

数组顺序即加载顺序，**不能颠倒**：

| 下标 | 文件 | 作用 | 驱动处理 |
|---|---|---|---|
| `[0]` | `gngbtnv20.bin` | NVM（校准/EEPROM 数据） | TLV 解析，写入 `nvm` |
| `[1]` | `gngbtfw20.mbn` | rampatch（固件本体） | `.mbn` 后缀 → `ELF_TYPE_PATCH`，实为 RISC-V ELF（"Peach" HAL），见 `btqca.c:840` |

构建脚本里有断言锁死这条链路：既检查两个 blob 真进了 `Image`，也检查编译出的
DTB 里 `qcom,wcn7850-bt` 的 `firmware-name` 与内嵌名一致。任一处对不上都会
静默退回 ROM 固件，所以必须在构建期就拦住，而不是等上机看 dmesg。

## 校验

见 `SHA256SUMS`：

| 文件 | 大小 | sha256 |
|---|---|---|
| `gngbtfw20.mbn` | 232436 B | `5f01e1337ba561eb…19e26` |
| `gngbtnv20.bin` | 13399 B | `b5a9a76227960972…cb6fe0` |

与设备端 `/lib/firmware/qca/` 下的同名文件逐字节一致。

## 出处

本仓库 `work/bt-firmware/bt-firmware/`，与 `rootfs-alpine/lib/firmware/qca/`
下的副本 sha256 相同（三方一致）。
