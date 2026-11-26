#ifndef CACHE_H
#define CACHE_H

#include "logfs.h"

struct data_cache_entry {
    uint64_t block;
    char data[4096];
    bool valid, dirty;
    uint64_t used;
};

struct inode_cache_entry {
    uint64_t ino;
    logfs_inode inode;
    bool valid, dirty;
    uint64_t used;
};

extern logfs_super super_cache;
extern logfs_checkpoint checkpoint_cache;

// The caller owns fd and closes it after cache_sync succeeds.
int cache_init(int fd);
void cache_reset();
int cache_read_data(uint64_t block, char *out);
int cache_read_inode(uint64_t ino, logfs_inode *out);
void cache_invalidate_data(uint64_t block);
void cache_invalidate_inode(uint64_t ino);
void cache_checkpoint_changed();
void cache_super_changed();

// Reserve first, build pointers using first_block, then append the record.
int cache_prepare_record(int nblocks, uint64_t *first_block);
int cache_append_record(uint32_t op, const char *payload, const int32_t *map,
                        int nblocks, uint64_t first_block);
int cache_sync();
int cache_free_segments();
int cache_segment_room();
int cache_current_segment();
int cache_read_segment_header(uint32_t segment, logfs_segment_hdr *out);

#endif
