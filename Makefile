# Linux GNU make. On FreeBSD use: make -C freebsd
# or: sudo ./install.sh

KDIR ?= /lib/modules/$(shell uname -r)/build
UNAME_S := $(shell uname -s)

.PHONY: all clean install uninstall test-encode test

ifeq ($(UNAME_S),FreeBSD)
all:
	$(MAKE) -C freebsd all

clean:
	$(MAKE) -C freebsd clean

install:
	$(MAKE) -C freebsd install
else
all:
	$(MAKE) -C hid-t150 KDIR="$(KDIR)" all

clean:
	$(MAKE) -C hid-t150 KDIR="$(KDIR)" clean

install:
	$(MAKE) -C hid-t150 KDIR="$(KDIR)" install
endif

uninstall:
	@echo "Run ./uninstall.sh as root to remove DKMS/udev files as well."

test-encode:
	@binary=$$(mktemp); trap 'unlink "$$binary"' EXIT; \
		$(CC) -O2 -Wall -Wextra -Werror -Icommon -o "$$binary" common/test_encode.c && "$$binary"

test: test-encode
	python -m unittest discover -s tests
