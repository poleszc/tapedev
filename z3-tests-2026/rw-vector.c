#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <stdint.h>

#define IOV_SIZE 8192*2
#define TAPEDEV_SECT_TAPESIZE_TO_BYTES(n)       (32 * (1 << (n)) * 8192)

// 32 * 8192
// 64 * 8192
// 128 * 8192
// 256 * 8192

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: %s [path_to_disk]\n", argv[0]);
        return -1;
    }

    srand(time(NULL));
    int rand_val = rand();

    // O_DIRECT?
    int fd = open(argv[1], O_SYNC | O_RDWR);
    if (fd < 0) {
        perror("failed to open file");
        return -1;
    }

    uint64_t blksize;
    int ret = ioctl(fd, BLKGETSIZE64, &blksize);
    if (ret < 0) {
        perror("failed to get size");
        return -1;
    }

    for (int size = 0; size != 4; size++) {
        uint64_t tapesize = TAPEDEV_SECT_TAPESIZE_TO_BYTES(size);
        if (tapesize > blksize) {
            printf("skipping size %lu > %lu\n", tapesize, blksize);
            continue;
        }

        struct iovec vec[8];

        for (int i=0; i != sizeof(vec)/sizeof(struct iovec); i++) {
            vec[i].iov_base = malloc(IOV_SIZE);
            vec[i].iov_len = IOV_SIZE;

            for (int j=0; j != (IOV_SIZE/sizeof(uint32_t)); j++)
                ((uint32_t*)vec[i].iov_base)[j] = 1000 * i + rand_val;
        }

        int result = pwritev(
            fd,
            vec,
            sizeof(vec)/sizeof(struct iovec),
            (tapesize-4*IOV_SIZE)
        );
        if (result < 0) {
            perror("failed to write");
            return -1;
        }

        for (int i=0; i != sizeof(vec)/sizeof(struct iovec); i++) {
            for (int j=0; j != (IOV_SIZE/sizeof(uint32_t)); j++)
                ((uint32_t*)vec[i].iov_base)[j] = 0;
        }

        result = preadv(
            fd,
            vec,
            sizeof(vec)/sizeof(struct iovec),
            (tapesize-4*IOV_SIZE)
        );
        if (result < 0) {
            perror("failed to read");
            return -1;
        }

        for (int i=0; i != sizeof(vec)/sizeof(struct iovec); i++) {
            for (int j=0; j != (IOV_SIZE/sizeof(uint32_t)); j++) {
                if (((uint32_t*)vec[i].iov_base)[j] != 1000 * i + rand_val) {
                    printf("fail\n");
                    return -1;
                }
            }
        }
    }
}
