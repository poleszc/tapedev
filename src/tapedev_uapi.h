#ifndef TAPEDEV_UAPI_H
#define TAPEDEV_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct tapedev_sect_info {
	__u32 tapes;
	__u32 tape_type;
	__u32 current_tape;
};

#define TAPEDEV_IOCTL_EJECT_TAPE _IO('~', 1)
#define TAPEDEV_IOCTL_GET_INFO   _IOR('~', 0, struct tapedev_sect_info)

#endif