PHONY += all autoversion kernel install_kernel install clean clean_kernel distclean install_modules install_scripts build_py_scripts help
	
all:
.PHONY: $(PHONY)

.DELETE_ON_ERROR:

include ./configure.mk.kernel

export COMPAT_AUTOCONF=$(CWD)/include/linux/compat_autoconf.h
export KSRC
export KSRC_OBJ
export KLIB_BUILD
export KVERSION
export MAKE


data_dir = /usr/share/mlnx_ofed

export CREL=$(shell cat $(CWD)/compat_version)
export CREL_PRE:=.compat_autoconf_
export CREL_CHECK:=$(CREL_PRE)$(CREL)
CFLAGS += \
	-DCOMPAT_BASE="\"$(shell cat compat_base)\"" \
	-DCOMPAT_BASE_TREE="\"$(shell cat compat_base_tree)\"" \
	-DCOMPAT_BASE_TREE_VERSION="\"$(shell cat compat_base_tree_version)\"" \
	-DCOMPAT_PROJECT="\"Compat-mlnx-ofed\"" \
	-DCOMPAT_VERSION="\"$(shell cat compat_version)\"" \

ifeq ($(CONFIG_COMPAT_TEST), y)
CFLAGS += -DCONFIG_COMPAT_TEST
endif

CONFIG_GCC_VERSION = $(shell ofed_scripts/get_gcc_version.sh)

CC  ?= $(CROSS_COMPILE)gcc
CPP ?= $(CC) -E

ifneq ($(CC),cc)
override WITH_MAKE_PARAMS += CC="$(CC)"
endif

# try-run
# Usage: option = $(call try-run, $(CC)...-o "$$TMP",option-ok,otherwise)
# Exit code chooses option. "$$TMP" is can be used as temporary file and
# is automatically cleaned up.
TMPOUT := /tmp/
try-run = $(shell set -e;				\
	TMP="$(TMPOUT).$$$$.tmp";		\
	TMPO="$(TMPOUT).$$$$.o";		\
	if ($(1)) >/dev/null 2>&1;		\
	then echo "$(2)";				\
	else echo "$(3)";				\
	fi;								\
	rm -f "$$TMP" "$$TMPO")

# cc-option
# Usage: cflags-y += $(call cc-option,-march=winchip-c6,-march=i586)
cc-option = $(call try-run,\
	$(CC) $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS) $(1) -c -x c /dev/null -o "$$TMP",$(1),$(2))

ifneq ($(call cc-option,-Werror=date-time),)
KCFLAGS += -Wno-date-time
export KCFLAGS
endif

CPP_MAJOR := $(shell $(CPP) -dumpversion 2>&1 | cut -d'.' -f1)
CPP_MINOR := $(shell $(CPP) -dumpversion 2>&1 | cut -d'.' -f2)
CPP_PATCH := $(shell $(CPP) -dumpversion 2>&1 | cut -d'.' -f3)
# Assumes that major, minor, and patch cannot exceed 999
CPP_VERS  := $(shell expr 0$(CPP_MAJOR) \* 1000000 + 0$(CPP_MINOR) \* 1000 + 0$(CPP_PATCH))

ifneq (,$(filter $(ARCH), x86_64))
MLNX_CFLAGS += $(shell if [ $(CPP_VERS) -ge 4004004 ]; then \
                   echo "-Werror"; else echo ""; fi)
endif #(,$(filter $(ARCH), i386 x86_64))
export MLNX_CFLAGS

ifneq ($(shell if (echo $(KVERSION) | grep -qE 'uek'); then \
					echo "YES"; else echo ""; fi),)
override WITH_MAKE_PARAMS += ctf-dir="$(CWD)/.ctf"
CFLAGS += -DMLX_DISABLE_TRACEPOINTS
endif

DEPMOD  = /sbin/depmod
INSTALL_MOD_DIR ?= $(shell ( test -f /etc/SuSE-release && echo updates ) || ( test -f /etc/SUSE-brand && echo updates ) || ( test -f /etc/fedora-release && echo updates ) || ( echo extra/mlnx-ofa_kernel ) )

ifeq ($(CONFIG_MEMTRACK),m)
        export KERNEL_MEMTRACK_CFLAGS = -include $(CWD)/drivers/infiniband/debug/mtrack.h
else
        export KERNEL_MEMTRACK_CFLAGS =
endif

export OPEN_ISCSI_MODULES = iscsi_tcp.ko libiscsi.ko scsi_transport_iscsi.ko

configure.mk.kernel:
	@echo Please run ./configure
	@exit 1

autoversion:
	@./ofed_scripts/autoversion.sh

all: kernel build_py_scripts

install: install_kernel install_scripts
install_kernel: install_modules

autoconf_h=$(shell /bin/ls -1 $(KSRC_OBJ)/include/*/autoconf.h 2> /dev/null | head -1)
kconfig_h=$(shell /bin/ls -1 $(KSRC_OBJ)/include/*/kconfig.h 2> /dev/null | head -1)

ifneq ($(kconfig_h),)
KCONFIG_H = -include $(kconfig_h)
endif

V ?= 0

LINUXINCLUDE=\
		-D__OFED_BUILD__ \
		-D__KERNEL__ \
		$(CFLAGS) \
		-include $(autoconf_h) \
		$(KCONFIG_H) \
		-include $(CWD)/include/linux/compat-2.6.h \
		$(BACKPORT_INCLUDES) \
		$(KERNEL_MEMTRACK_CFLAGS) \
		$(OPENIB_KERNEL_EXTRA_CFLAGS) \
		-I$(CWD)/include \
		-I$(CWD)/include/uapi \
		-I$(CWD)/drivers/infiniband/debug \
		$$(if $$(CONFIG_XEN),-D__XEN_INTERFACE_VERSION__=$$(CONFIG_XEN_INTERFACE_VERSION)) \
		$$(if $$(CONFIG_XEN),-I$$(srctree)/arch/x86/include/mach-xen) \
		-I$$(srctree)/arch/$$(SRCARCH)/include \
		-I$$(objtree)/arch/$$(SRCARCH)/include/generated \
		-I$$(objtree)/include \
		-I$$(srctree)/arch/$$(SRCARCH)/include/uapi \
		-I$$(objtree)/arch/$$(SRCARCH)/include/generated/uapi \
		-I$$(srctree)/include \
		-I$$(srctree)/include/uapi \
		-I$$(objtree)/include/generated/uapi \
		$$(if $$(KBUILD_SRC),-I$$(objtree)/include2 -I$$(srctree)/include) \
		-I$$(srctree)/arch/$$(SRCARCH)/include \
		-I$$(objtree)/arch/$$(SRCARCH)/include/generated \
		#

#########################
#	make kernel	#
#########################
#NB: The LINUXINCLUDE value comes from main kernel Makefile
#    with local directories prepended. This eventually affects
#    CPPFLAGS in the kernel Makefile
kernel: autoversion
	@echo "Building kernel modules"
	@echo "Kernel version: $(KVERSION)"
	@echo "Modules directory: $(INSTALL_MOD_PATH)/lib/modules/$(KVERSION)/$(INSTALL_MOD_DIR)"
	@echo "Kernel build: $(KSRC_OBJ)"
	@echo "Kernel sources: $(KSRC)"
	env CWD=$(CWD) BACKPORT_INCLUDES=$(BACKPORT_INCLUDES) \
		$(MAKE) -C $(KSRC_OBJ) M="$(CWD)" \
		V=$(V) KBUILD_NOCMDDEP=1 $(WITH_MAKE_PARAMS) \
		CONFIG_COMPAT_VERSION=$(CONFIG_COMPAT_VERSION) \
		CONFIG_COMPAT_KOBJECT_BACKPORT=$(CONFIG_COMPAT_KOBJECT_BACKPORT) \
		CONFIG_MEMTRACK=$(CONFIG_MEMTRACK) \
		CONFIG_DEBUG_INFO=$(CONFIG_DEBUG_INFO) \
		CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=y \
		CONFIG_INFINIBAND=$(CONFIG_INFINIBAND) \
		CONFIG_INFINIBAND_CORE_DUMMY=$(CONFIG_INFINIBAND_CORE_DUMMY) \
		CONFIG_INFINIBAND_IPOIB=$(CONFIG_INFINIBAND_IPOIB) \
		CONFIG_IPOIB_VERSION=$(CONFIG_IPOIB_VERSION) \
		CONFIG_INFINIBAND_IPOIB_CM=$(CONFIG_INFINIBAND_IPOIB_CM) \
		CONFIG_IPOIB_ALL_MULTI=$(CONFIG_IPOIB_ALL_MULTI) \
                CONFIG_INFINIBAND_MLNX_BX=$(CONFIG_INFINIBAND_MLNX_BX) \
		CONFIG_INFINIBAND_SRP=$(CONFIG_INFINIBAND_SRP) \
		CONFIG_INFINIBAND_SRP_DUMMY=$(CONFIG_INFINIBAND_SRP_DUMMY) \
		CONFIG_RDMA_RXE=$(CONFIG_RDMA_RXE) \
		CONFIG_RDMA_RXE_DUMMY=$(CONFIG_RDMA_RXE_DUMMY) \
		CONFIG_NVME_CORE=$(CONFIG_NVME_CORE) \
		CONFIG_NVME_HOST_WITHOUT_FC=$(CONFIG_NVME_HOST_WITHOUT_FC) \
		CONFIG_BLK_DEV_NVME=$(CONFIG_BLK_DEV_NVME) \
		CONFIG_NVME_FABRICS=$(CONFIG_NVME_FABRICS) \
		CONFIG_NVME_FC=$(CONFIG_NVME_FC) \
		CONFIG_NVME_RDMA=$(CONFIG_NVME_RDMA) \
		CONFIG_NVME_TCP=$(CONFIG_NVME_TCP) \
		CONFIG_NVME_APPLE=$(CONFIG_NVME_APPLE) \
		CONFIG_NVME_HOST_AUTH=$(CONFIG_NVME_HOST_AUTH) \
		CONFIG_NVME_MULTIPATH=$(CONFIG_NVME_MULTIPATH) \
		CONFIG_NVME_HOST_DUMMY=$(CONFIG_NVME_HOST_DUMMY) \
		CONFIG_NVME_TARGET=$(CONFIG_NVME_TARGET) \
		CONFIG_NVME_TARGET_LOOP=$(CONFIG_NVME_TARGET_LOOP) \
		CONFIG_NVME_TARGET_RDMA=$(CONFIG_NVME_TARGET_RDMA) \
		CONFIG_NVME_TARGET_TCP=$(CONFIG_NVME_TARGET_TCP) \
		CONFIG_NVME_TARGET_FC=$(CONFIG_NVME_TARGET_FC) \
		CONFIG_NVME_TARGET_FCLOOP=$(CONFIG_NVME_TARGET_FCLOOP) \
		CONFIG_NVME_TARGET_DUMMY=$(CONFIG_NVME_TARGET_DUMMY) \
		CONFIG_NVME_KEYRING=$(CONFIG_NVME_KEYRING) \
		CONFIG_NVME_AUTH=$(CONFIG_NVME_AUTH) \
		CONFIG_SCSI_SRP_ATTRS=$(CONFIG_SCSI_SRP_ATTRS) \
		CONFIG_INFINIBAND_USER_MAD=$(CONFIG_INFINIBAND_USER_MAD) \
		CONFIG_INFINIBAND_USER_ACCESS=$(CONFIG_INFINIBAND_USER_ACCESS) \
		CONFIG_INFINIBAND_USER_ACCESS_UCM=$(CONFIG_INFINIBAND_USER_ACCESS_UCM) \
		CONFIG_INFINIBAND_USER_MEM=$(CONFIG_INFINIBAND_USER_MEM) \
		CONFIG_INFINIBAND_ADDR_TRANS=$(CONFIG_INFINIBAND_ADDR_TRANS) \
		CONFIG_INFINIBAND_ADDR_TRANS_CONFIGFS=$(CONFIG_INFINIBAND_ADDR_TRANS_CONFIGFS) \
		CONFIG_INFINIBAND_IPOIB_DEBUG=$(CONFIG_INFINIBAND_IPOIB_DEBUG) \
		CONFIG_INFINIBAND_ISERT=$(CONFIG_INFINIBAND_ISERT) \
		CONFIG_INFINIBAND_ISERT_DUMMY=$(CONFIG_INFINIBAND_ISERT_DUMMY) \
		CONFIG_INFINIBAND_ISER=$(CONFIG_INFINIBAND_ISER) \
		CONFIG_INFINIBAND_ISER_DUMMY=$(CONFIG_INFINIBAND_ISER_DUMMY) \
		CONFIG_SCSI_ISCSI_ATTRS=$(CONFIG_SCSI_ISCSI_ATTRS) \
		CONFIG_ISCSI_TCP=$(CONFIG_ISCSI_TCP) \
		CONFIG_INFINIBAND_IPOIB_DEBUG_DATA=$(CONFIG_INFINIBAND_IPOIB_DEBUG_DATA) \
		CONFIG_INFINIBAND_SDP_SEND_ZCOPY=$(CONFIG_INFINIBAND_SDP_SEND_ZCOPY) \
		CONFIG_INFINIBAND_SDP_RECV_ZCOPY=$(CONFIG_INFINIBAND_SDP_RECV_ZCOPY) \
		CONFIG_INFINIBAND_SDP_DEBUG=$(CONFIG_INFINIBAND_SDP_DEBUG) \
		CONFIG_INFINIBAND_SDP_DEBUG_DATA=$(CONFIG_INFINIBAND_SDP_DEBUG_DATA) \
		CONFIG_INFINIBAND_MADEYE=$(CONFIG_INFINIBAND_MADEYE) \
		CONFIG_MLXFW=$(CONFIG_MLXFW) \
		CONFIG_MLX5_CORE=$(CONFIG_MLX5_CORE) \
		CONFIG_MLX5_ACCEL=$(CONFIG_MLX5_ACCEL) \
		CONFIG_MLX5_EN_TLS=$(CONFIG_MLX5_EN_TLS) \
		CONFIG_MLX5_TLS=$(CONFIG_MLX5_TLS) \
		CONFIG_MLX5_FPGA_TLS=$(CONFIG_MLX5_FPGA_TLS) \
		CONFIG_MLX5_FPGA_IPSEC=$(CONFIG_MLX5_FPGA_IPSEC) \
		CONFIG_MLX5_IPSEC=$(CONFIG_MLX5_IPSEC) \
		CONFIG_MLX5_EN_IPSEC=$(CONFIG_MLX5_EN_IPSEC) \
		CONFIG_MLX5_FPGA=$(CONFIG_MLX5_FPGA) \
		CONFIG_MLX5_CORE_EN=$(CONFIG_MLX5_CORE_EN) \
		CONFIG_MLX5_CORE_EN_DCB=$(CONFIG_MLX5_CORE_EN_DCB) \
		CONFIG_MLX5_EN_ARFS=$(CONFIG_MLX5_EN_ARFS) \
		CONFIG_MLX5_EN_RXNFC=$(CONFIG_MLX5_EN_RXNFC) \
		CONFIG_MLX5_MPFS=$(CONFIG_MLX5_MPFS) \
		CONFIG_MLX5_ESWITCH=$(CONFIG_MLX5_ESWITCH) \
		CONFIG_MLX5_CLS_ACT=$(CONFIG_MLX5_CLS_ACT) \
		CONFIG_MLX5_BRIDGE=$(CONFIG_MLX5_BRIDGE) \
		CONFIG_MLX5_TC_CT=$(CONFIG_MLX5_TC_CT) \
		CONFIG_MLX5_SF=$(CONFIG_MLX5_SF) \
		CONFIG_MLX5_SF_MANAGER=$(CONFIG_MLX5_SF_MANAGER) \
		CONFIG_MLX5_SW_STEERING=$(CONFIG_MLX5_SW_STEERING) \
		CONFIG_MLX5_HW_STEERING=$(CONFIG_MLX5_HW_STEERING) \
		CONFIG_MLX5_CORE_IPOIB=$(CONFIG_MLX5_CORE_IPOIB) \
		CONFIG_MLX5_INFINIBAND=$(CONFIG_MLX5_INFINIBAND) \
		CONFIG_MLX5_DEBUG=$(CONFIG_MLX5_DEBUG) \
		CONFIG_AUXILIARY_BUS=$(CONFIG_AUXILIARY_BUS) \
		CONFIG_FWCTL=$(CONFIG_FWCTL) \
		CONFIG_FWCTL_MLX5=$(CONFIG_FWCTL_MLX5) \
		CONFIG_MLX5_GCOV=$(CONFIG_MLX5_GCOV) \
		CONFIG_SUNRPC_XPRT_RDMA=$(CONFIG_SUNRPC_XPRT_RDMA) \
		CONFIG_SUNRPC_XPRT_RDMA_DUMMY=$(CONFIG_SUNRPC_XPRT_RDMA_DUMMY) \
		CONFIG_DTRACE= \
		CONFIG_CTF= \
		CONFIG_INFINIBAND_ON_DEMAND_PAGING=$(CONFIG_INFINIBAND_ON_DEMAND_PAGING) \
		CONFIG_BF_DEVICE_EMULATION=$(CONFIG_BF_DEVICE_EMULATION) \
		CONFIG_BF_POWER_FAILURE_EVENT=$(CONFIG_BF_POWER_FAILURE_EVENT) \
		CONFIG_GPU_DIRECT_STORAGE=$(CONFIG_GPU_DIRECT_STORAGE) \
		CONFIG_VIRTIO_FS=$(CONFIG_VIRTIO_FS) \
		CONFIG_FUSE_PASSTHROUGH=$(CONFIG_FUSE_PASSTHROUGH) \
		CONFIG_FUSE_DAX=$(CONFIG_FUSE_DAX) \
		CONFIG_ENABLE_MLX5_FS_DEBUGFS=$(CONFIG_ENABLE_MLX5_FS_DEBUGFS) \
		CONFIG_MLXDEVM=$(CONFIG_MLXDEVM) \
		CONFIG_MLX5_SF_CFG=$(CONFIG_MLX5_SF_CFG) \
		CONFIG_MLX5_TC_SAMPLE=$(CONFIG_MLX5_TC_SAMPLE) \
		CONFIG_MLX5_MACSEC=$(CONFIG_MLX5_MACSEC) \
		CONFIG_MLX5_DPLL=$(CONFIG_MLX5_DPLL) \
		CONFIG_GCC_VERSION=$(CONFIG_GCC_VERSION) \
		CONFIG_IS_MARINER=$(CONFIG_IS_MARINER) \
		CONFIG_IS_AZURELINUX=$(CONFIG_IS_AZURELINUX) \
		LINUXINCLUDE='$(LINUXINCLUDE)' \
		modules


#########################
#	Install kernel	#
#########################
install_modules:
	@echo "Installing kernel modules"

ifeq ($(shell /usr/bin/lsb_release -s -i 2>/dev/null | grep -qiE "debian|ubuntu" 2>/dev/null && echo YES || echo ''),)
	$(MAKE) -C $(KSRC_OBJ) M="$(CWD)" \
		INSTALL_MOD_PATH=$(INSTALL_MOD_PATH) \
		INSTALL_MOD_DIR=$(INSTALL_MOD_DIR) \
		$(WITH_MAKE_PARAMS) modules_install;
else
	install -d $(INSTALL_MOD_PATH)/lib/modules/$(KVERSION)/updates/dkms/
	find $(CWD)/ \( -name "*.ko" -o -name "*.ko.gz" \) -exec /bin/cp -f '{}' $(INSTALL_MOD_PATH)/lib/modules/$(KVERSION)/updates/dkms/ \;
endif

	if [ ! -n "$(INSTALL_MOD_PATH)" ]; then $(DEPMOD) $(KVERSION);fi;


#########################
#	Install scripts	 #
#########################
install_scripts:
	@echo "Installing scripts and configuration files"
	install -d $(INSTALL_MOD_PATH)/etc/init.d
	install -d $(INSTALL_MOD_PATH)$(data_dir)
ifeq ($(shell /usr/bin/lsb_release -s -i 2>/dev/null | grep -qiE "debian|ubuntu" 2>/dev/null && echo YES || echo ''),)
	install -m 0755 ofed_scripts/openibd $(INSTALL_MOD_PATH)/etc/init.d
else
	install -m 0755 ofed_scripts/mlnx-ofed-kernel-utils.openibd.init $(INSTALL_MOD_PATH)/etc/init.d/openibd
endif

	@echo "Running install helper script"
	ofed_scripts/install_helper $(INSTALL_MOD_PATH)

	install -d $(INSTALL_MOD_PATH)/sbin
	install -d $(INSTALL_MOD_PATH)/$(prefix)/sbin
	install -m 0755 ofed_scripts/mlnx_bf_assign_ct_cores.sh $(INSTALL_MOD_PATH)$(data_dir)
	install -m 0755 ofed_scripts/mlnx_drv_ctl $(INSTALL_MOD_PATH)$(data_dir)
	install -m 0755 ofed_scripts/mod_load_funcs $(INSTALL_MOD_PATH)$(data_dir)

	install -d $(INSTALL_MOD_PATH)/etc/modprobe.d
ifeq ("$(wildcard $(INSTALL_MOD_PATH)/etc/modprobe.d/mlnx.conf)","")
	install -m 0644 ofed_scripts/mlnx.conf $(INSTALL_MOD_PATH)/etc/modprobe.d
endif

ifeq ("$(wildcard $(INSTALL_MOD_PATH)/etc/modprobe.d/mlnx-bf.conf)","")
	install -m 0644 ofed_scripts/mlnx-bf.conf $(INSTALL_MOD_PATH)/etc/modprobe.d
endif

ifneq ($(filter m,$(CONFIG_MLX5_CORE)),)
	install -d $(INSTALL_MOD_PATH)/$(prefix)/bin
	install -m 0755 ofed_scripts/ibdev2netdev $(INSTALL_MOD_PATH)/$(prefix)/sbin
endif

ifeq ($(CONFIG_INFINIBAND_IPOIB),m)
	install -d $(INSTALL_MOD_PATH)/etc/modprobe.d
ifeq ("$(wildcard $(INSTALL_MOD_PATH)/etc/modprobe.d/ib_ipoib.conf)","")
	install -m 0644 ofed_scripts/ib_ipoib.conf $(INSTALL_MOD_PATH)/etc/modprobe.d
endif
endif

	install -d $(INSTALL_MOD_PATH)/lib/udev/rules.d
	install -m 0755 ofed_scripts/auxdev-sf-netdev-rename $(INSTALL_MOD_PATH)/lib/udev
	install -m 0755 ofed_scripts/sf-rep-netdev-rename $(INSTALL_MOD_PATH)/lib/udev
	install -m 0644 ofed_scripts/83-mlnx-sf-name.rules $(INSTALL_MOD_PATH)/lib/udev/rules.d
	install -m 0644 ofed_scripts/90-ib.rules $(INSTALL_MOD_PATH)/lib/udev/rules.d

	install -d $(INSTALL_MOD_PATH)/usr/sbin
	install -m 0755 ofed_scripts/setup_mr_cache.sh $(INSTALL_MOD_PATH)/usr/sbin
	install -d $(INSTALL_MOD_PATH)/etc/


clean: clean_kernel

clean_kernel:
	$(MAKE) -C $(KSRC_OBJ) rm-files= M="$(CWD)" $(WITH_MAKE_PARAMS) clean

distclean: clean_kernel
	@/bin/rm -f $(clean-files)
	@/bin/rm -rf $(PWD)/rpm-dist

clean-files := Module.symvers modules.order Module.markers compat/modules.order
clean-files += $(COMPAT_CONFIG) $(COMPAT_AUTOCONF)

help:
	@echo
	@echo "kernel:                      Build kernel modules"
	@echo "all:                         Run kernel and build_py_scripts"
	@echo
	@echo "install_kernel:              Install kernel modules under $(INSTALL_MOD_PATH)/lib/modules/$(KVERSION)/$(INSTALL_MOD_DIR)"
	@echo "install_scripts:             Install scripts and configurations files"
	@echo "install:                     Run install_kernel and install_scripts"
	@echo
	@echo "clean_kernel:                Delete kernel modules binaries"
	@echo "clean:                       Run clean_kernel"
	@echo "distclean:                   Run clean_kernel and also remove auto-generated files"
	@echo
