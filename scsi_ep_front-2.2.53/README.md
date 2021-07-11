# Huawei SD-100 cinder-scsi driver

This SCSI device driver exposes the SCSI (metadata.hw:passthrough=true) cinder volumes on Bare Metal (Ironic) servers in Open Telekom Cloud.
The device driver supports specific Huawei SDI add-in cards (SD100EP - 19e5:1610).

## Requirements

- kernel-headers
- kernel-devel
- dos2unix
- elfutils-libelf-devel
- binutils

```shell
# SUSE based
$ sudo zypper in kernel-source kernel-default-devel dos2unix binutils make gcc

# yum/dnf based
$ sudo dnf install kernel-headers kernel-devel dos2unix binutils make gcc

# debain based
$ sudo apt install linux-headers-$(uname -r) linux-source dos2unix binutils make gcc
```

```shell
# build kernel module
$ make 

# clean up directory
$ make clean

$ [sudo] modinfo ./scsi_ep_front.ko
```

Sometimes there is a missing or defect symlink under `/lib/modules/$(uname -r)/` for `build`:

```shell
$ cd /lib/modules/$(uname -r)/
$ ls -al
$ sudo unlink build
$ sudo ln -s /usr/src/kernels/$(uanme -r) build
```

## Target of this clone repo

- Reorganisation of the original repository https://github.com/SDI-Drivers/scsi_ep_front.
- Fixing some errors and make the kernel module compatibel with newer kernels.

---

> SD100 Card Driver Project version: SD100 V100R001C00SPC106B010 

The following is the readme file of SD100 Card driver. From this file, you will learn how to build SD100 kernel driver and kernel rpm install package.

## Prepare the Environment
+ Install the rpmbuild package
+ Install kernel development enviroment package
> e.g. kernel version of suse11 sp4 is 3.0.101-100, we should install ***kernel-source-3.0.101-100.1.x86_64.rpm*** and ***kernel-default-devel-3.0.101-100.1.x86_64.rpm*** these two packages.
+ Intall gcc
+ Modify the configuration file: ***/usr/lib/rpm/find-requires.ksyms*** as shown below	

```Bash
case "$1" in 
kernel-module-*)    ;;
kernel*)            is_kernel_package=1 ;;  
scsi_ep_front)      is_kernel_package=1 ;; #add this line in the file  
esac
```

## Compile the Driver

+ Enter the ***scsi_ep_front*** directory
+ Replace **Makefile** with tempalte file **Makefile_*your_os_name***, if your OS isn't in the makefile list, you should create one tmeplate by yourslef
> **NOTE:** Makfile_Suse and Makefile_redhat are difference. We **CANN'T** use Makefile_Suse as template to build reahat rpm package.
+ Open Makefile and change ***KERNELDIR*** to the *kernel source directory* in your OS. ***KERNELINST*** must be the ***/lib/modules/\`uname -r\`/*** directory 
> e.g.  when we build suse 11sp4 which kernel version is 3.0.101-100-default kernel driver,we should modify Makefile as shown below:

```Makefile
KERNELDIR ?= /lib/modules/3.0.101-100-default/build/
KERNELINST ?= /lib/modules/3.0.101-100-default/
```

+ build rpm package as follows commands.

```Bash
make clean	
make srpm
```

The rpm package will be build in the ./x86_64/ directory
+ compile the drivers as follows commands.

```Bash
make clean	
make
```

The kernel driver ***scsi_ep_front.ko*** will be create in current directory.

