
# To build modules outside of the kernel tree, we run "make"
# in the kernel source tree; the Makefile these then includes this
# Makefile once again.
# This conditional selects whether we are being included from the
# kernel Makefile or not.

DOLPHIN := /opt/DIS

EXTRA_CFLAGS := -I$(DOLPHIN)/DIS/src/IRM_GX/drv/src -I$(DOLPHIN)/DIS/src/IRM_GX/drv/src/LINUX -I$(DOLPHIN)/DIS/src/COMMON/osif/kernel/include -I$(DOLPHIN)/DIS/src/COMMON/osif/kernel/include/LINUX -I$(DOLPHIN)/DIS/src/include/ -I$(DOLPHIN)/DIS/src/include/dis/ -I/home/helios/mklinux-arm-x86/include/
EXTRA_CFLAGS += -I/opt/DIS/include/ 
EXTRA_CFLAGS += -D_KERNEL -DOS_IS_LINUX -D_DIS_KERNEL_

KBUILD_EXTRA_SYMBOLS := ./Module.symvers 

ifeq ($(KERNELRELEASE),)

    # Assume the source tree is where the running kernel was built
    # You should set KERNELDIR in the environment if it's elsewhere
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    # The current directory is passed to sub-makes as argument
    PWD := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions  modules.order

.PHONY: modules modules_install clean

else
    # called from kernel build system: just declare what our modules are
    obj-m := msg_layer.o
endif


