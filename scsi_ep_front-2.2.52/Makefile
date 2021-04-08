CUR := $(shell pwd)
KDIR=/lib/modules/`uname -r`/build
obj-m += scsi_ep_front.o
scsi_ep_front-y += epfront_main.o epfront_transfer.o epfront_sysfs.o
DRV_VER := $(shell [ -f $(obj)/buildversion ] && cat $(obj)/buildversion)
EXTRA_CFLAGS += -DSDI_MULTI_CARD
EXTRA_CFLAGS += -DDRV_VERSION='"$(DRV_VER)"'
EXTRA_CFLAGS += -g
EXTRA_CFLAGS += -D_GNU_SOURCE

DRIVER_NAME := scsi_ep_front

all:
	make LC_ALL=C ARCH=x86_64 CROSS_COMPILE= -C $(KDIR) M=$(CUR) modules
clean:
	make LC_ALL=C ARCH=x86_64 CROSS_COMPILE= -C $(KDIR) M=$(CUR) clean

