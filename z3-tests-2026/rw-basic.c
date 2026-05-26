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

    uint64_t blksize;
    int ret = ioctl(fd, BLKGETSIZE64, &blksize);
    if (ret < 0) {
        perror("failed to get size");
        return -1;
    }

    int bsizes[6] = {512, 1024, 2048, 4096, 8192, blksize};
    int offsets[6] = {0, 1, 32*8192-511, 64*8192-511, 128*8192-511, 256*8192-511};
    unsigned char* buf = malloc(blksize);
    unsigned char* result_buf = malloc(blksize);
    if (buf == NULL || result_buf == NULL) {
        perror("failed to alocate");
        return -1;
    }

    for (int i=0; i != sizeof(bsizes)/sizeof(int); i++) {
        for (int j=0; j != sizeof(offsets)/sizeof(int); j++) {
            unsigned char val = rand() % 255;
            int size = bsizes[i];
            int off = offsets[j];

            if (size+off >= blksize) {
                continue;
            }

            for (uint64_t i=0; i != size; i++)
                buf[i] = val;

            int result = lseek(fd, off, SEEK_SET);
            if (result < 0) {
                perror("failed to lseek");
                return -1;
            }

            result = write(fd, buf, size);
            if (result < 0) {
                perror("failed to write");
                return -1;
            }

            result = lseek(fd, off, SEEK_SET);
            if (result < 0) {
                perror("failed to lseek");
                return -1;
            }

            result = read(fd, result_buf, size);
            if (result < 0) {
                perror("failed to write");
                return -1;
            }

            for (uint64_t i=0; i != size; i++) {
                if (result_buf[i] != val) {
                    printf("invalid byte %d != %d on %lu with size %d\n", result_buf[i], val, i, size);
                    return -1;
                }
            }
        }
    }
}
