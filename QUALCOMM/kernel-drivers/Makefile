# We should probably use subdir-ccflags-y for this.  However, that gets added
# to the gcc command after the standard include.  Gcc will parse the include
# directories in the order that they are listed.  This means that our include
# directory gets last preference, and we cannot override any headers that may
# be provided by the kernel we are compiling against.  This is not what we want
# so instead, we abuse the kbuild system a bit to have our include directory be
# listed first.
NOSTDINC_FLAGS += -I$(src)/include

obj-y := mhi/ qaic/

QAIC_BACKPORT_FLAGS_FILE := $(src)/qaic_backport_flags
QAIC_BACKPORT_FLAGS := $(shell cat $(QAIC_BACKPORT_FLAGS_FILE))

KBUILD_CFLAGS_MODULE += ${QAIC_BACKPORT_FLAGS} -DCONFIG_DRM_QAIC_HWMON -DCONFIG_MHI_BUS_DEBUG
$(info ${KBUILD_CFLAGS_MODULE})
QAIC_MODULES = CONFIG_MHI_BUS=m CONFIG_MHI_BUS_DEBUG=y CONFIG_DRM_ACCEL_QAIC=m CONFIG_MHI_BUS_PCI_GENERIC=n CONFIG_MHI_BUS_EP=n CONFIG_DRM_QAIC_HWMON=y

default:
	./make.sh $$KERNELRELEASE "$(MAKE) -C /lib/modules/$$KERNELRELEASE/build/ M=$$PWD $(QAIC_MODULES)"

