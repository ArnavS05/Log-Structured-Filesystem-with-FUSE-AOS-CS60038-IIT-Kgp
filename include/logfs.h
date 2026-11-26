#ifndef LOGFS_H
#define LOGFS_H

#include <cstdint>
#include <cstddef>

#define LOGFS_MAGIC 0x4C4F4746U
#define LOGFS_BLOCK_SIZE 4096U
#define LOGFS_SEG_BLOCKS 64U
#define LOGFS_MAX_INODES 1024U
#define LOGFS_MAX_SEGMENTS 1024U
#define LOGFS_NDIRECT 12
#define SEG_MAGIC 0x5345474DU
#define REC_MAGIC 0x52454352U
#define SEG_SEALED 1U

const uint64_t LOGFS_IMAGE_SIZE=256ULL * 1024 *1024;
const uint64_t LOGFS_TOTAL_BLOCKS =LOGFS_IMAGE_SIZE /LOGFS_BLOCK_SIZE;
const uint64_t LOGFS_CHECKPOINT_BLOCK=1;
const uint64_t LOGFS_CHECKPOINT_BLOCKS =3;
const uint64_t LOGFS_LOG_START=4;
const uint32_t LOGFS_NUM_SEGMENTS =(LOGFS_TOTAL_BLOCKS-LOGFS_LOG_START) / LOGFS_SEG_BLOCKS;
const int LOGFS_REC_OFFSET=4080;

struct logfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t segment_blocks;
    uint32_t max_inodes;
    uint32_t num_segments;
    uint64_t total_blocks;
    uint64_t checkpoint_block;
    uint64_t log_start_block;
    uint32_t crc32;
    char pad[4044];
};

struct logfs_checkpoint {
    uint64_t imap[LOGFS_MAX_INODES];
    uint8_t inode_bitmap[LOGFS_MAX_INODES];
    uint8_t segment_bitmap[LOGFS_MAX_SEGMENTS];
    uint32_t crc32;
    char pad[2044];
};

struct logfs_segment_hdr {
    uint32_t magic;
    uint32_t seg_seq;
    uint32_t flags;
    int32_t block_map[LOGFS_SEG_BLOCKS];
    uint32_t crc32;
    char pad[3824];
};

struct logfs_dirent {
    uint64_t ino;
    uint16_t name_len;
    char name[255];
    char pad[7];
};

struct logfs_inode {
    uint64_t ino;
    uint32_t mode;
    uint32_t reserved;
    uint64_t size;
    int64_t mtime;
    uint64_t direct[LOGFS_NDIRECT];
    char pad[3968];
};

enum logfs_op {
    OP_CREATE=1,
    OP_MKDIR =2,
    OP_WRITE=3,
    OP_RMDIR =4
};

struct logfs_rec_hdr {
    uint32_t magic;
    uint32_t op;
    uint32_t nblocks;
    uint32_t crc32;
};

// These checks stop the compiler from silently changing our disk layout.
static_assert(sizeof(logfs_super)==4096, "superblock size");
static_assert(offsetof(logfs_super, crc32)==48, "superblock crc offset");
static_assert(sizeof(logfs_checkpoint)==12288, "checkpoint size");
static_assert(offsetof(logfs_checkpoint, crc32)==10240, "checkpoint crc offset");
static_assert(sizeof(logfs_segment_hdr)==4096, "segment header size");
static_assert(offsetof(logfs_segment_hdr, crc32)==268, "segment crc offset");
static_assert(sizeof(logfs_inode)==4096, "inode size");
static_assert(offsetof(logfs_inode, direct)==32, "inode direct offset");
static_assert(sizeof(logfs_dirent)==272, "directory entry size");
static_assert(sizeof(logfs_rec_hdr)==16, "record header size");

uint32_t calc_crc(const void *data, size_t size);
void set_bit(uint8_t *bitmap, int bit);
int write_region(int fd, const void *data, size_t size, uint64_t block);
int read_region(int fd, void *data, size_t size, uint64_t block);
int get_bit(const uint8_t *bitmap, int bit);
void clear_bit(uint8_t *bitmap, int bit);
int logfs_fsync(int fd);

#endif
