obj-m += logfs_stats.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules

clean:
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions

.PHONY: all clean
