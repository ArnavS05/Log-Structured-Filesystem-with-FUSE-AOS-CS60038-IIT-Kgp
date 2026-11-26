#include "logfs.h"
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstring>

uint32_t calc_crc(const void *data, size_t size){
    const uint8_t *p=(const uint8_t *)data;
    uint32_t crc =0xffffffffU;

    for(size_t i=0;i <size;i++){
        crc ^=p[i];
        for(int j=0;j <8;j++){
            if(crc &1) crc=(crc >>1) ^0xedb88320U;
            else crc =crc>>1;
        }
    }
    return crc ^0xffffffffU;
}

void set_bit(uint8_t *bitmap, int bit){
    bitmap[bit /8] |=1U <<(bit%8);
}

int get_bit(const uint8_t *bitmap, int bit){
    return (bitmap[bit/8] >> (bit%8)) &1;
}

void clear_bit(uint8_t *bitmap, int bit){
    bitmap[bit /8] &=~(1U <<(bit%8));
}

int read_region(int fd, void *data, size_t size, uint64_t block){
    char *p=(char *)data;
    size_t done =0;
    while(done<size){
        ssize_t n =pread(fd, p+done, size-done, block*LOGFS_BLOCK_SIZE+done);
        if(n<0){
            if(errno ==EINTR) continue;
            return -errno;
        }
        if(n==0) return -EIO;
        done+=n;
    }
    return 0;
}

int write_region(int fd, const void *data, size_t size, uint64_t block){
    const char *p =(const char *)data;
    size_t done=0;

    while(done <size){
        ssize_t n=pwrite(fd, p+done, size-done, block*LOGFS_BLOCK_SIZE+done);
        if(n <0){
            if(errno==EINTR) continue;
            return -errno;
        }
        if(n==0) return -EIO;
        done +=n;
    }
    return 0;
}

int logfs_fsync(int fd){
    int ret;
    do { ret=fsync(fd); } while(ret<0 &&errno==EINTR);
    if(ret<0) return -errno;

    int notify=open("/sys/kernel/logfs_stats/notify", O_WRONLY |O_CLOEXEC);
    if(notify<0){
        if(errno!=ENOENT &&errno!=ENOTDIR)
            fprintf(stderr, "logfs_stats notify: %s\n", strerror(errno));
        return 0; // The image was synced even if statistics are unavailable.
    }
    ssize_t n;
    do { n=write(notify, "1\n", 2); } while(n<0 &&errno==EINTR);
    if(n!=2){
        int error=n<0 ?errno :EIO;
        fprintf(stderr, "logfs_stats notify: %s\n", strerror(error));
    }
    close(notify);
    return 0;
}
