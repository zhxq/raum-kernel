CC := gcc-11

# Name of the final module
MOD_NAME := dm-biza
obj-m += $(MOD_NAME).o

# List all the object files that make up your module
$(MOD_NAME)-y := dm-biza-target.o dm-biza-pred.o dm-biza-map.o dm-biza-gc.o

# Path to the currently running kernel's build directory
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
DEST := /lib/modules/$(shell uname -r)/kernel/drivers/md/

EXTRA_CFLAGS += -g -DDEBUG

all:
	$(MAKE) -C $(KDIR) M=$(PWD) CC=$(CC) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	@echo "Installing $(MOD_NAME) to $(DEST)..."
	install -d $(DEST)
	install -m 644 $(MOD_NAME).ko $(DEST)
	@echo "Updating module dependencies..."
	depmod -a
	@echo "Done. You can now load it with 'modprobe $(MOD_NAME)'"

# Uninstall the module
uninstall:
	@echo "Removing $(MOD_NAME) from $(DEST)..."
	rm -f $(DEST)$(MOD_NAME).ko
	depmod -a
	@echo "Uninstalled."

.PHONY: all clean install uninstall