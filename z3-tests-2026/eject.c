#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <stdint.h>

#define TAPEDEV_IOCTL_EJECT_TAPE           _IO('~', 1)

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: %s [path_to_disk]\n", argv[0]);
        return -1;
    }

    srand(time(NULL));

    int fd = open(argv[1], O_SYNC | O_RDWR);
    if (fd < 0) {
        perror("failed to open file");
        return -1;
    }

    int result = ioctl(fd, TAPEDEV_IOCTL_EJECT_TAPE, NULL);
    if (result < 0) {
        perror("failed to eject by ioctl");
        return -1;
    }

    return 0;
}
