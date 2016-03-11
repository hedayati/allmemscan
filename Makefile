obj-m += allmem.o

all: allmemscan
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

allmemscan: allmemscan.c
	cc -O2 -Wall -g -pthread allmemscan.c -o allmemscan

clean:
	rm -rf allmemscan
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

