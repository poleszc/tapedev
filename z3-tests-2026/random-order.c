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

#define NUM_OPS 16
#define MAX_SIZE 2*8192
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
    int pos;
    int size;
    char* write_buf;
    char* result_buf;
    bool read_done;
} operation;

// https://stackoverflow.com/a/10072899
void shuffle(int* arr, operation* ops, int size)
{
    for (int i=size-1; i > 0; i--) {
        int j = rand() % (i+1);
        int tmp = arr[j];
        arr[j] = arr[i];
        arr[i] = tmp;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: %s [path_to_disk]\n", argv[0]);
        return -1;
    }

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

    srand(time(NULL));
    operation ops[NUM_OPS];
    int pos[NUM_OPS];

    for (int i=0; i != NUM_OPS; i++) {
        ops[i].pos = rand() % (blksize-MAX_SIZE);
        ops[i].size = rand() % MAX_SIZE;
        ops[i].read_done = false;
        ops[i].write_buf = (char*)malloc(ops[i].size);
        ops[i].result_buf = (char*)malloc(ops[i].size);

        for (int j=0; j < i; j++) {
            // i starts after j, but j covers part of i
            if (ops[i].pos > ops[j].pos && ops[i].pos < (ops[j].pos + ops[j].size)) {
                ops[i].size -= MAX(0, (ops[j].pos + ops[j].size) - ops[i].pos);
                ops[i].pos = ops[j].pos + ops[j].size;
                // printf("new pos and size %d %d %d\n", i, ops[i].pos, ops[i].size);
            }

            // i starts before j, but i covers part of j
            if (ops[i].pos < ops[j].pos && (ops[i].pos + ops[i].size) > ops[j].pos) {
                ops[i].size = MAX(0, ops[j].pos - ops[i].pos);
                // printf("new size %d %d %d\n", i, ops[i].pos, ops[i].size);
            }
        }

        for (int j=0; j < ops[i].size; j++)
            ops[i].write_buf[j] = i;

        pos[i] = i;
    }

    shuffle(pos, ops, NUM_OPS);

    for (int i=0; i != NUM_OPS; i++) {
        int result = lseek(fd, ops[pos[i]].pos, SEEK_SET);
        if (result < 0) {
            perror("failed to lseek");
            return -1;
        }

        if (ops[pos[i]].size <= 0)
            continue;

        result = write(fd, ops[pos[i]].write_buf, ops[pos[i]].size);
        if (result < 0) {
            perror("failed to write");
            return -1;
        }
    }

    shuffle(pos, ops, NUM_OPS);

    for (int i=0; i != NUM_OPS; i++) {
        int result = lseek(fd, ops[pos[i]].pos, SEEK_SET);
        if (result < 0) {
            perror("failed to lseek");
            return -1;
        }

        if (ops[pos[i]].size <= 0)
            continue;

        result = read(fd, ops[pos[i]].result_buf, ops[pos[i]].size);
        if (result < 0) {
            perror("failed to read");
            return -1;
        }

        for (uint64_t j=0; j != ops[pos[i]].size; j++) {
            if (ops[pos[i]].write_buf[j] != ops[pos[i]].result_buf[j]) {
                printf("fail %d %d\n", ops[pos[i]].write_buf[j], ops[pos[i]].result_buf[j]);
                return -1;
            }
        }
    }
}
