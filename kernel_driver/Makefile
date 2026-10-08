obj-m += ext_mem.o

KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(CURDIR)

all:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" clean

install:
	insmod "$(MODULE_DIR)/ext_mem.ko"
	chmod 600 /dev/ext_mem

uninstall:
	rmmod ext_mem

.PHONY: all clean install uninstall
