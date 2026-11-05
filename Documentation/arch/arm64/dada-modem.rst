.. SPDX-License-Identifier: GPL-2.0

Xiaomi 15 (dada) modem configuration
==================================

``dada_defconfig`` follows the modem-enabled configuration validated on
SM8750/dada in November 2026. It forces the built-in
``qcom/sm8750-xiaomi-dada-modem`` device tree. That board variant enables
MPSS and IPA, allocates guarded RMTFS memory below 4 GiB, and keeps the
shared WLAN supplies at the device's required minimum voltages.

Kernel prerequisites
--------------------

The default configuration includes:

* QRTR/GLINK, QMI, SMEM and the in-kernel PD mapper for modem services.
* Modular Q6V5 PAS remoteproc support to boot MPSS/ADSP with userspace
  controlling firmware and service startup order.
* Modular IPA and RMNET for the cellular data path.
* SHMBRIDGE TZ memory and modular TEE/QCOMTEE for the security listeners.
* UFS SCSI BSG for the current RPMB listener transport. ``CONFIG_RPMB``
  is not needed by that userspace backend.
* USB gadget/configfs support matching the validated USB device setup.
* Modular dada AudioReach transport, voice backend and microphone mute
  add-ons under ``CONFIG_SND_SOC_DADA_VOICE_CONTROLS``.

``CONFIG_QCOM_DADA_MODEM_DIAG`` builds shared diagnostic tracepoints
independently of QRTR's module configuration. Recording requires tracing
support at build time (``CONFIG_FTRACE`` and
``CONFIG_ENABLE_DEFAULT_TRACERS``), then
explicit tracefs enablement; the default keeps FTRACE disabled. Verbose
lifecycle messages use dynamic debug when available. The privileged ``dump_only``
debugfs command accepts ``collect`` only for an already crashed MPSS with
recovery disabled, buffered coredumps enabled and exactly one power vote.
It stops and dumps without rebooting the modem. It is a diagnostic facility,
not part of normal service startup or a validated modem recovery procedure.

Build and install
-----------------

From the kernel source directory, use an AArch64 cross toolchain::

    make O=../build-dada ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dada_defconfig
    make O=../build-dada ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image modules qcom/sm8750-xiaomi-dada-modem.dtb
    make O=../build-dada ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_install INSTALL_MOD_PATH=/path/to/new-rootfs

Embedded firmware paths remain relative to the source tree's ``firmware/``.
Supply the blobs listed in ``CONFIG_EXTRA_FIRMWARE`` before building; they
are not supplied by these modem patches. In particular, the voice transport
requires ``qcom/sm8750/Xiaomi-dada-tplg.bin`` with SHA-256
``d6dcd361d4cd6784fc20cdf02e37278c532cf3dbd127630fa3bd72e3093cce92``.
Use the project's device-specific boot image packaging procedure for Image;
``modules_install`` alone does not install a bootable phone image.

The default does not retain the dated local version of the previously
flashed image. Install all modules built alongside the new Image under its
own kernel release; do not mix in modules from the old image.

Runtime MPSS/ADSP and IPA firmware, including all MDT segments, must also be
available in the new rootfs before those remote processors are started. The
modem device tree requests ``qcom/sm8750/modem.mdt``,
``qcom/sm8750/modem_dtb.mdt`` and ``qcom/sm8750/dada/ipa_fws.mdt``.

Load ``dada_audioreach``, ``dada_voice_backend`` and ``dada_call_mute`` after
ADSP, APM and the Xiaomi-dada sound card are ready. Their names and ioctl
layouts match the previously validated external modules. Replace the old
external copies when using the in-tree modules, so modprobe cannot select
two builds of the same module. The AudioReach transport and microphone
control remain loaded until reboot. A timeout may retain DSP-owned buffers
or backend references; hot unloading or restarting a DSP is not a cleanup
procedure.

Rootfs requirements
-------------------

A kernel with this configuration still requires the device port's
userspace stack: firmware staging, qrtr-ns, TFTP, RMTFS, security/RPMB
listeners, the repaired MINK/securefs component, IMS DCM and IMS daemons,
the repaired ModemManager, NetworkManager, audiopd and the dada voice audio
service. These programs and their systemd/D-Bus/udev configuration are
maintained outside this kernel repository. Distribution ModemManager alone
does not contain the port's DSDS initialization repairs. The SMS service
path also relies on the modem/IMS userspace setup.

NetworkManager/ModemManager manage registration and data sessions. IMS and
the voice service supply calling; the AudioReach add-ons provide the DSP
control path rather than carrying calls through ordinary desktop audio
streams. The voice service must coordinate PCM ownership and restore the
desktop audio configuration on hangup. GNOME or KDE can share this kernel
and service stack, with session audio, D-Bus permissions and UI integration
adapted to the chosen rootfs.
