CFILES = getRss.c

obj-m := GetRss.o
GetRss-objs := $(CFILES:.c=.o)

build:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

install:
	sudo insmod GetRss.ko
	sudo mknod /dev/GetRss c 64 1
	sudo chmod 666 /dev/GetRss

remove:
	sudo rmmod GetRss.ko
	sudo rm /dev/GetRss
