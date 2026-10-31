# SPDX-License-Identifier: GPL-2.0
#
# Synaptics TouchComm touchscreen driver
# (Xiaomi dada round CN mainline port of the vendor synaptics_tcm driver)
#

obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics-tcm.o

synaptics-tcm-y := syna_tcm2.o \
	   syna_tcm2_platform_spi.o \
	   tcm/synaptics_touchcom_core_v1.o \
	   tcm/synaptics_touchcom_core_v2.o \
	   tcm/synaptics_touchcom_func_base.o \
	   tcm/synaptics_touchcom_func_touch.o

ccflags-y += -I$(src)/tcm
