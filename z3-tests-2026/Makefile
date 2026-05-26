all:  eject random-order rw-basic rw-vector ioctl-read rw-direct zig-zag-order backward-order

backward-order: backward-order.c
	gcc backward-order.c -o backward-order

eject: eject.c
	gcc eject.c -o eject

ioctl-read: ioctl-read.c
	gcc ioctl-read.c -o ioctl-read

random-order: random-order.c
	gcc random-order.c -o random-order

rw-basic: rw-basic.c
	gcc rw-basic.c -o rw-basic

rw-direct: rw-direct.c
	gcc rw-direct.c -o rw-direct

rw-vector: rw-vector.c
	gcc rw-vector.c -o rw-vector

zig-zag-order: zig-zag-order.c
	gcc zig-zag-order.c -o zig-zag-order

clean:
	rm -f eject random-order rw-basic rw-vector ioctl-read rw-direct zig-zag-order backward-order

