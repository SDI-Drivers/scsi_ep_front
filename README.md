# Huawei_SDI
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

