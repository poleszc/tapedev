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

    uint64_t devsize;
    uint64_t blksize = 2*8192;
    int ret = ioctl(fd, BLKGETSIZE64, &devsize);
    if (ret < 0) {
        perror("failed to get size");
        return -1;
    }

    uint32_t cnt = rand();
    uint32_t* buf = malloc(blksize);

    for (int i=0; i < devsize/blksize; i++) {
        int result = lseek(fd, -(i+1)*blksize, SEEK_END);
        if (result < 0) {
            perror("failed to lseek");
            return -1;
        }

        for (int j=0; j != blksize/sizeof(cnt); j++) {
            buf[j] = cnt + i;
        }

        result = write(fd, buf, blksize);
        if (result < 0) {
            perror("failed to write");
            return -1;
        }
    }

    for (int i=0; i < devsize/blksize; i++) {
        int lseek_result = lseek(fd, -(i+1)*blksize, SEEK_END);
        if (lseek_result < 0) {
            perror("failed to lseek");
            return -1;
        }

        for (int j=0; j != blksize/sizeof(cnt); j++) {
            buf[j] = 0;
        }

        int result = read(fd, buf, blksize);
        if (result < 0) {
            perror("failed to read");
            return -1;
        }

        for (int j=0; j != blksize/sizeof(cnt); j++) {
            if (buf[j] != cnt + i) {
                printf("invalid read value pos %d %u %u", lseek_result, buf[j], cnt+i);
                return -1;
            }
        }
    }
}
