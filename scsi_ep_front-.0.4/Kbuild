# for driver version begin
DRV_BUILD_TIME := $(shell date +"%Y-%m-%d %H:%M:%S")
ifeq ($(DRV_BUILD_TIME), )
    DRV_BUILD_TIME := '--'
endif

SVN := $(shell command -v svn > /dev/null 2>&1 && echo "1")
ifeq ($(SVN), 1)
    SVN_VERSION := $(shell svn info $(obj) | grep '^Last Changed Rev' | cut -d ' ' -f4)
endif
ifeq ($(SVN_VERSION), )
    SVN_VERSION := '--'
endif

DRV_DES := $(shell echo "BUILD_TIME[$(DRV_BUILD_TIME)], SVN[$(SVN_VERSION)]")
DRV_VER := $(shell [ -f $(obj)/buildversion ] && cat $(obj)/buildversion)
ifeq ($(DRV_VER), )
    DRV_VER := '--'
endif
EXTRA_CFLAGS += -DDRV_VERSION='"$(DRV_VER)"' -DDRV_DESCRIPTION='"${DRV_DES}"'
# for driver version end
obj-m += scsi_ep_front.o
scsi_ep_front-objs := epfront_transfer.o epfront_main.o epfront_sysfs.o
