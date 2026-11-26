#include "logfs.h"
#include <iostream>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
using namespace std;

int format_image(int fd){
    logfs_super sb={};
    logfs_checkpoint cp ={};
    logfs_segment_hdr seg={};
    logfs_inode root ={};
    logfs_rec_hdr rec={};

    sb.magic=LOGFS_MAGIC;
    sb.version =1;
    sb.block_size=LOGFS_BLOCK_SIZE;
    sb.segment_blocks =LOGFS_SEG_BLOCKS;
    sb.max_inodes=LOGFS_MAX_INODES;
    sb.num_segments =LOGFS_NUM_SEGMENTS;
    sb.total_blocks=LOGFS_TOTAL_BLOCKS;
    sb.checkpoint_block =LOGFS_CHECKPOINT_BLOCK;
    sb.log_start_block=LOGFS_LOG_START;
    sb.crc32 =calc_crc(&sb, offsetof(logfs_super, crc32));

    // Segment 0 begins at block 4; its first body block is block 5.
    cp.imap[0]=LOGFS_LOG_START+1;
    set_bit(cp.inode_bitmap, 0);
    set_bit(cp.segment_bitmap, 0);
    cp.crc32=calc_crc(&cp, offsetof(logfs_checkpoint, crc32));

    seg.magic =SEG_MAGIC;
    seg.seg_seq=1;
    seg.flags =0; // This segment is still open.
    for(int i=0;i <64;i++) seg.block_map[i]=-1;
    seg.block_map[1] =-2;
    seg.crc32=calc_crc(&seg, offsetof(logfs_segment_hdr, crc32));

    root.ino=0;
    root.mode =S_IFDIR |0755;
    root.size=0;
    root.mtime =time(nullptr);

    // Header lives in the unused tail of this inode block.
    rec.magic=REC_MAGIC;
    rec.op =OP_MKDIR;
    rec.nblocks=1;
    char *block =(char *)&root;
    memcpy(block+LOGFS_REC_OFFSET, &rec, sizeof(rec));
    rec.crc32=calc_crc(block, LOGFS_BLOCK_SIZE);
    memcpy(block+LOGFS_REC_OFFSET, &rec, sizeof(rec));

    // Write a complete segment. The other body blocks start empty.
    char buffer[LOGFS_SEG_BLOCKS *LOGFS_BLOCK_SIZE]={};
    memcpy(buffer, &seg, sizeof(seg));
    memcpy(buffer+LOGFS_BLOCK_SIZE, &root, sizeof(root));

    int ret=write_region(fd, buffer, sizeof(buffer), LOGFS_LOG_START);
    if(ret <0) return ret;
    ret =write_region(fd, &cp, sizeof(cp), LOGFS_CHECKPOINT_BLOCK);
    if(ret<0) return ret;
    ret=write_region(fd, &sb, sizeof(sb), 0);
    if(ret <0) return ret;
    return logfs_fsync(fd);
}

int main(int argc, char **argv){
    if(argc>2){
        cerr <<"Usage: "<<argv[0] <<" [image_path]\n";
        return 1;
    }

    uint16_t one=1;
    if(*(uint8_t *)&one !=1){
        cerr<<"This implementation needs a little-endian machine.\n";
        return 1;
    }

    const char *path="logfs.img";
    if(argc ==2) path=argv[1];

    // Do not accidentally overwrite an existing image.
    int fd =open(path, O_RDWR |O_CREAT |O_EXCL, 0644);
    if(fd<0){
        cerr <<"open: "<<strerror(errno)<<"\n";
        return 1;
    }

    int ret=0;
    if(ftruncate(fd, LOGFS_IMAGE_SIZE)<0) ret =-errno;
    else ret=format_image(fd);

    if(close(fd)<0 &&ret==0) ret=-errno;
    if(ret <0){
        cerr<<"format: " <<strerror(-ret)<<"\n";
        unlink(path);
        return 1;
    }

    cout <<"Created "<<path<<" (256 MiB)\n";
    cout<<"Blocks: " <<LOGFS_TOTAL_BLOCKS<<"\n";
    cout <<"Segments: "<<LOGFS_NUM_SEGMENTS <<"\n";
    cout<<"Root inode: 0, stored at block 5\n";
    return 0;
}
