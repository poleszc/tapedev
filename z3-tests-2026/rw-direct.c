// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <sys/syscall.h>
#include <stdint.h>

#define O_DIRECT	00040000

// https://elixir.bootlin.com/linux/v6.18.5/source/samples/vfs/test-statx.c
ssize_t statx(int dfd, const char *filename, unsigned flags,
	      unsigned int mask, struct statx *buffer)
{
	return syscall(__NR_statx, dfd, filename, flags, mask, buffer);
}

int test_rw(int fd, int size, int off, int align, bool may_einval)
{
    unsigned char val = rand() % 255;
    int ret = -1;

    unsigned char* buf = aligned_alloc(align, size);
    unsigned char* result_buf = aligned_alloc(align, size);

    if (buf == NULL || result_buf == NULL) {
        perror("failed to alocate");
        goto err;
    }

    for (uint64_t i=0; i != size; i++)
        buf[i] = val;

    int result = lseek(fd, off, SEEK_SET);
    if (result < 0) {
        perror("failed to lseek");
        goto err;
    }

    result = write(fd, buf, size);
    if (result < 0) {
        if (may_einval && errno == EINVAL) {
            printf("failed to write with einval but einval allowed (that's ok)\n");
            goto ret;
        }
        perror("failed to write");
        goto err;
    }

    result = lseek(fd, off, SEEK_SET);
    if (result < 0) {
        perror("failed to lseek");
        goto err;
    }

    result = read(fd, result_buf, size);
    if (result < 0) {
        if (may_einval && errno == EINVAL) {
            printf("failed to read with einval but einval allowed (that's ok)\n");
            goto ret;
        }
        perror("failed to read");
        goto err;
    }

    for (uint64_t i=0; i != size; i++) {
        if (result_buf[i] != val) {
            printf("invalid byte %d != %d on %lu with size %d\n", result_buf[i], val, i, size);
            goto err;
        }
    }

ret:
    ret = 0;

err:
    free(buf);
    free(result_buf);

    return ret;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: %s [path_to_disk]\n", argv[0]);
        return -1;
    }

    struct statx stats;
    int result = statx(AT_FDCWD, argv[1], 0, STATX_DIOALIGN, &stats);
    if (result < 0) {
        perror("failed to statx");
        return -1;
    }

    printf("alignment: %u %u\n", stats.stx_dio_mem_align, stats.stx_dio_offset_align);

    srand(time(NULL));

    int fd = open(argv[1], O_SYNC | O_RDWR | O_DIRECT);
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

    if (test_rw(fd, stats.stx_dio_mem_align, 0, stats.stx_dio_mem_align-1, false) < 0)
        return -1;

    if (test_rw(fd, stats.stx_dio_offset_align, stats.stx_dio_offset_align, stats.stx_dio_mem_align-1, false) < 0)
        return -1;

    int alignments[] = {0, 512, 1024, 2048, 4096, 8192};
    int offsets[] = {0, 1, 512, 1024, 2048, 4096, 8192};

    for (int a=0; a != sizeof(alignments)/sizeof(int); a++) {
        for (int o=0; o != sizeof(alignments)/sizeof(int); o++) {
            printf("alignment: %d offset: %d\n", alignments[a], offsets[o]);
            if (test_rw(fd, stats.stx_dio_mem_align, offsets[o], alignments[a], true) < 0)
                return -1;
        }
    }
}
