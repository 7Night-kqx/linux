# GPU 固件（Adreno a8xx / SM8750 dada）

4 个文件供内核 `CONFIG_EXTRA_FIRMWARE` 编进 `.rodata` 使用。

## 为什么必须编进内核

`msm_dpu` 在 initcall（`do_basic_setup()`，早于 `prepare_namespace()` 挂 rootfs）阶段
就请求 `qcom/gen80000_sqe.fw`，那时 rootfs 还没挂上，`request_firmware()` 必然
`-ENOENT`（日志 `Direct firmware load for qcom/gen80000_sqe.fw failed with error -2`）。
所以这些必须编进内核，而不是等 rootfs。

## 用法（内核树内）

```
CONFIG_EXTRA_FIRMWARE="qcom/gen80000_sqe.fw qcom/gen80000_gmu.bin qcom/gen80000_aqe.fw qcom/gen80000_zap.mbn"
CONFIG_EXTRA_FIRMWARE_DIR="<此仓库 clone>/firmware"
```

## 校验

见 SHA256SUMS。

## 各文件出处

### `gen80000_zap.mbn` — 本机提取，已校验

直接从本机 `dada`（model `24129PN74C`，ROM `CP2A.260605.016` / `OS4.0.0.16.XOCCNXM`）
的 `/odm/firmware/gen80000_zap.mbn` 取出：

```
adb shell 'su -c "cat /odm/firmware/gen80000_zap.mbn"' > firmware/qcom/gen80000_zap.mbn
```

12088 字节，sha256 `7d935a98…b0ec`，与设备侧 `sha256sum` 逐字节一致。
ELF32 / `QUALCOMM DSP6` / `EXEC`，无 section header，唯一 `PT_LOAD`：
`paddr=0x1000 memsz=0x3b4`，entry `0x1000`。
即 `qcom_mdt_get_size()` 返回 `0x1000`（4096 字节）。

A830 的 `adreno_device`（`a6xx_catalog.c`）没有 `.zapfw` 成员，所以 zap 不能走
默认的 `<chip>.zapfw` 命名，必须由 DT 节点的 `firmware-name` 显式给出。

### 前 3 个 — 仓库既有，已实机验证可用，**未**替换

`gen80000_sqe.fw` / `gen80000_gmu.bin` / `gen80000_aqe.fw` 是本仓库早已存在的文件，
它们的原始出处**未能核实**，此处不再沿用旧版 README 里
"设备 vendor 固件 / rootfs 的 `/usr/lib/firmware/qcom/`" 的说法（该说法与实测不符）。

与本机当前 ROM 内固件的实测对比：

| 文件 | 本仓库 | 本机 ROM | 关系 |
|---|---|---|---|
| `gen80000_gmu.bin` | 88896 B / `1ad8175e…` | `/vendor/firmware/gen80000_gmu.bin` 88896 B / `1ad8175e…` | **完全一致** |
| `gen80000_sqe.fw` | 111476 B / `30ee3301…` | `/vendor/firmware/gen80000_sqe.fw` 111892 B / `9c8f009b…` | **不同** |
| `gen80000_aqe.fw` | 25044 B / `67562adb…` | `/vendor/firmware/gen80000_aqe.fw` 25340 B / `f04cb97e…` | 尺寸/哈希不同（内容前缀一致，见下） |

**决策：不换。** 这 3 个已在本机主线内核上实测 probe 成功
（`Loaded GMU firmware v5.1.25`，三件均 `loaded ... from new location`，`renderD128` 就位），
且 14 档 OPP 频率与 11 个 `acd-level` 与 stock `qcom,gpu-pwrlevels-2`（speed-bin `0xe8`）
**逐字节一致**，说明本仓库这套就是匹配该 die 的版本。换成本机 ROM 那套属于无收益变更，
反而引入新变量。本机 ROM 的 SQE/AQE 哈希在此登记备查。
