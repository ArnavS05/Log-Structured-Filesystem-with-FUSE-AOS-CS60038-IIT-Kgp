#include "cache.h"
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>

logfs_super super_cache;
logfs_checkpoint checkpoint_cache;

static data_cache_entry data_cache[64];
static inode_cache_entry inode_cache[64];
static char segment_buffer[64 *4096];
static int image_fd=-1, current_segment =-1, next_slot=1;
static uint32_t sequence=0;
static uint64_t clock_tick =0;
static bool buffer_dirty=false, checkpoint_dirty =false, super_dirty=false;

void cache_reset(){
    memset(data_cache, 0, sizeof(data_cache));
    memset(inode_cache, 0, sizeof(inode_cache));
    memset(&super_cache, 0, sizeof(super_cache));
    memset(&checkpoint_cache, 0, sizeof(checkpoint_cache));
    memset(segment_buffer, 0, sizeof(segment_buffer));
    image_fd=-1;
    current_segment =-1;
    next_slot=1;
    sequence =0;
    clock_tick=0;
    buffer_dirty =checkpoint_dirty=super_dirty=false;
}

static uint64_t segment_start(int s){
    return super_cache.log_start_block+s *LOGFS_SEG_BLOCKS;
}

int cache_free_segments(){
    if(image_fd<0) return -EBADF;
    int count=0;
    for(uint32_t i=0;i<super_cache.num_segments;i++)
        if(!get_bit(checkpoint_cache.segment_bitmap, i)) count++;
    return count;
}

int cache_segment_room(){
    if(image_fd<0) return -EBADF;
    return current_segment<0 ?0 :64-next_slot;
}

int cache_current_segment(){ return current_segment; }

int cache_read_segment_header(uint32_t segment, logfs_segment_hdr *out){
    if(image_fd<0) return -EBADF;
    if(!out ||segment>=super_cache.num_segments) return -EINVAL;
    if(!get_bit(checkpoint_cache.segment_bitmap, segment)) return -ENOENT;
    if((int)segment==current_segment) memcpy(out, segment_buffer, sizeof(*out));
    else{
        int ret=read_region(image_fd, out, sizeof(*out), segment_start(segment));
        if(ret<0) return ret;
    }
    if(out->magic!=SEG_MAGIC ||out->crc32!=calc_crc(out, 268)) return -EIO;
    return 0;
}

static int valid_block(uint64_t block){
    if(block<super_cache.log_start_block) return 0;
    uint64_t relative=block-super_cache.log_start_block;
    return relative <super_cache.num_segments*64ULL &&relative%64 !=0;
}

static int in_buffer(uint64_t block){
    if(current_segment<0) return 0;
    uint64_t start=segment_start(current_segment);
    return block>start &&block<start+next_slot;
}

static void save_data(uint64_t block, const char *data, bool dirty){
    int slot=-1;
    for(int i=0;i <64;i++){
        if(data_cache[i].valid &&data_cache[i].block==block){
            slot=i;
            break;
        }
    }
    if(slot==-1){
        slot=0;
        for(int i=0;i<64;i++){
            if(!data_cache[i].valid){
                slot =i;
                break;
            }
            if(data_cache[i].used <data_cache[slot].used) slot=i;
        }
    }
    data_cache[slot].block =block;
    memcpy(data_cache[slot].data, data, 4096);
    data_cache[slot].valid=true;
    data_cache[slot].dirty =dirty;
    data_cache[slot].used=++clock_tick;
}

static void save_inode(const logfs_inode *inode, bool dirty){
    int slot =-1;
    for(int i=0;i<64;i++){
        if(inode_cache[i].valid &&inode_cache[i].ino==inode->ino){
            slot=i;
            break;
        }
    }
    if(slot==-1){
        slot=0;
        for(int i=0;i<64;i++){
            if(!inode_cache[i].valid){
                slot=i;
                break;
            }
            if(inode_cache[i].used<inode_cache[slot].used) slot =i;
        }
    }
    inode_cache[slot].ino=inode->ino;
    inode_cache[slot].inode =*inode;
    inode_cache[slot].valid=true;
    inode_cache[slot].dirty=dirty;
    inode_cache[slot].used =++clock_tick;
}

static int load_block(uint64_t block, void *out){
    if(!valid_block(block)) return -EINVAL;
    if(in_buffer(block)){
        uint64_t slot=block-segment_start(current_segment);
        memcpy(out, segment_buffer+slot*4096, 4096);
        return 0;
    }
    return read_region(image_fd, out, 4096, block);
}

int cache_read_data(uint64_t block, char *out){
    if(image_fd<0) return -EBADF;
    if(!out ||!valid_block(block)) return -EINVAL;
    for(int i=0;i<64;i++){
        if(data_cache[i].valid &&data_cache[i].block ==block){
            memcpy(out, data_cache[i].data, 4096);
            data_cache[i].used=++clock_tick;
            return 0;
        }
    }
    int ret =load_block(block, out);
    if(ret==0) save_data(block, out, in_buffer(block) &&buffer_dirty);
    return ret;
}

int cache_read_inode(uint64_t ino, logfs_inode *out){
    if(image_fd <0) return -EBADF;
    if(!out ||ino>=1024) return -EINVAL;
    if(!get_bit(checkpoint_cache.inode_bitmap, ino) ||checkpoint_cache.imap[ino]==0)
        return -ENOENT;
    for(int i=0;i <64;i++){
        if(inode_cache[i].valid &&inode_cache[i].ino==ino){
            *out=inode_cache[i].inode;
            inode_cache[i].used =++clock_tick;
            return 0;
        }
    }
    uint64_t block =checkpoint_cache.imap[ino];
    int ret=load_block(block, out);
    if(ret <0) return ret;
    if(out->ino!=ino) return -EIO;
    save_inode(out, in_buffer(block) &&buffer_dirty);
    return 0;
}

void cache_invalidate_data(uint64_t block){
    for(int i=0;i<64;i++)
        if(data_cache[i].valid &&data_cache[i].block==block)
            data_cache[i].valid =false;
}

void cache_invalidate_inode(uint64_t ino){
    for(int i=0;i <64;i++)
        if(inode_cache[i].valid &&inode_cache[i].ino==ino)
            inode_cache[i].valid=false;
}

void cache_checkpoint_changed(){ checkpoint_dirty =true; }
void cache_super_changed(){ super_dirty=true; }

static int check_records(const logfs_segment_hdr *hdr){
    int slot=1;
    while(slot<64 &&hdr->block_map[slot]!=-1){
        if(hdr->block_map[slot]!=-2) return -EIO;
        char *start=segment_buffer+slot*4096;
        logfs_rec_hdr rec;
        memcpy(&rec, start+LOGFS_REC_OFFSET, sizeof(rec));
        if(rec.magic!=REC_MAGIC ||rec.op<OP_CREATE ||rec.op>OP_RMDIR ||
           rec.nblocks==0 ||rec.nblocks>(uint32_t)(64-slot)) return -EIO;
        for(uint32_t i=0;i<rec.nblocks;i++){
            int tag=hdr->block_map[slot+i];
            if(tag==-2){
                uint64_t ino;
                memcpy(&ino, start+i*4096, sizeof(ino));
                if(ino>=1024) return -EIO;
            }else if(tag<0 ||tag>=1024) return -EIO;
        }
        uint32_t stored=rec.crc32;
        rec.crc32 =0;
        memcpy(start+LOGFS_REC_OFFSET, &rec, sizeof(rec));
        uint32_t computed=calc_crc(start, rec.nblocks*4096);
        rec.crc32=stored;
        memcpy(start+LOGFS_REC_OFFSET, &rec, sizeof(rec));
        if(stored!=computed) return -EIO;
        slot+=rec.nblocks;
    }
    for(int i=slot;i<64;i++)
        if(hdr->block_map[i]!=-1) return -EIO;
    return 0;
}

int cache_init(int fd){
    if(image_fd>=0) return -EBUSY;
    cache_reset();
    uint16_t one=1;
    if(*(uint8_t *)&one!=1) return -ENOTSUP;
    struct stat st;
    if(fstat(fd, &st)<0) return -errno;
    if(st.st_size !=(off_t)LOGFS_IMAGE_SIZE) return -EINVAL;

    int ret=read_region(fd, &super_cache, sizeof(super_cache), 0);
    if(ret<0) return ret;
    if(super_cache.crc32!=calc_crc(&super_cache, 48)) return -EIO;
    if(super_cache.magic!=LOGFS_MAGIC ||super_cache.version !=1 ||
       super_cache.block_size!=4096 ||super_cache.segment_blocks!=64 ||
       super_cache.max_inodes!=1024 ||super_cache.num_segments!=LOGFS_NUM_SEGMENTS ||
       super_cache.total_blocks!=LOGFS_TOTAL_BLOCKS ||
       super_cache.checkpoint_block!=1 ||super_cache.log_start_block!=4) return -EINVAL;

    ret =read_region(fd, &checkpoint_cache, sizeof(checkpoint_cache), 1);
    if(ret<0) return ret;
    if(checkpoint_cache.crc32 !=calc_crc(&checkpoint_cache, 10240)) return -EIO;
    if(!get_bit(checkpoint_cache.inode_bitmap, 0) ||checkpoint_cache.imap[0]==0)
        return -EIO;

    for(uint32_t s=0;s<super_cache.num_segments;s++){
        if(!get_bit(checkpoint_cache.segment_bitmap, s)) continue;
        logfs_segment_hdr hdr;
        ret=read_region(fd, &hdr, sizeof(hdr), segment_start(s));
        if(ret<0) return ret;
        if(hdr.magic!=SEG_MAGIC ||hdr.crc32 !=calc_crc(&hdr, 268)) return -EIO;
        if(hdr.flags &~SEG_SEALED) return -EIO;
        ret=read_region(fd, segment_buffer, sizeof(segment_buffer), segment_start(s));
        if(ret<0) return ret;
        ret=check_records(&hdr);
        if(ret<0) return ret;
        if(hdr.seg_seq>sequence) sequence=hdr.seg_seq;
        if(hdr.flags &SEG_SEALED) continue;
        if(current_segment>=0) return -EIO;
        current_segment =s;
        next_slot=1;
        while(next_slot<64 &&hdr.block_map[next_slot]!=-1) next_slot++;
        for(int i=next_slot;i<64;i++)
            if(hdr.block_map[i]!=-1) return -EIO;
    }
    if(current_segment>=0){
        ret=read_region(fd, segment_buffer, sizeof(segment_buffer),
                        segment_start(current_segment));
        if(ret<0) return ret;
    }
    image_fd=fd;
    logfs_inode root;
    ret =cache_read_inode(0, &root);
    if(ret<0 ||!(S_ISDIR(root.mode))){
        cache_reset();
        return ret<0 ?ret :-EIO;
    }
    return 0;
}

int cache_sync(){
    if(image_fd<0) return -EBADF;
    bool written=buffer_dirty ||checkpoint_dirty ||super_dirty;
    if(buffer_dirty){
        logfs_segment_hdr hdr;
        memcpy(&hdr, segment_buffer, sizeof(hdr));
        hdr.flags |=SEG_SEALED;
        hdr.crc32=calc_crc(&hdr, 268);
        memcpy(segment_buffer, &hdr, sizeof(hdr));
        int ret=write_region(image_fd, segment_buffer, sizeof(segment_buffer),
                             segment_start(current_segment));
        if(ret<0) return ret;
    }
    if(checkpoint_dirty){
        checkpoint_cache.crc32 =calc_crc(&checkpoint_cache, 10240);
        int ret=write_region(image_fd, &checkpoint_cache, sizeof(checkpoint_cache), 1);
        if(ret <0) return ret;
    }
    if(super_dirty){
        super_cache.crc32=calc_crc(&super_cache, 48);
        int ret =write_region(image_fd, &super_cache, sizeof(super_cache), 0);
        if(ret<0) return ret;
    }
    if(written){
        int ret=logfs_fsync(image_fd);
        if(ret<0) return ret;
    }
    if(buffer_dirty){
        current_segment=-1;
        next_slot =1;
    }
    buffer_dirty=checkpoint_dirty =super_dirty=false;
    for(int i=0;i<64;i++){
        data_cache[i].dirty =false;
        inode_cache[i].dirty=false;
    }
    return 0;
}

static int new_segment(){
    for(uint32_t s=0;s<super_cache.num_segments;s++){
        if(get_bit(checkpoint_cache.segment_bitmap, s)) continue;
        if(sequence==UINT32_MAX) return -EOVERFLOW;
        for(int i=1;i<64;i++) cache_invalidate_data(segment_start(s)+i);
        memset(segment_buffer, 0, sizeof(segment_buffer));
        logfs_segment_hdr hdr ={};
        hdr.magic=SEG_MAGIC;
        hdr.seg_seq =++sequence;
        for(int i=0;i<64;i++) hdr.block_map[i]=-1;
        hdr.crc32 =calc_crc(&hdr, 268);
        memcpy(segment_buffer, &hdr, sizeof(hdr));
        current_segment=s;
        next_slot=1;
        set_bit(checkpoint_cache.segment_bitmap, s);
        buffer_dirty=checkpoint_dirty =true;
        return 0;
    }
    return -ENOSPC;
}

int cache_prepare_record(int nblocks, uint64_t *first_block){
    if(image_fd <0) return -EBADF;
    if(!first_block ||nblocks<1 ||nblocks>63) return -EINVAL;
    if(current_segment>=0 &&next_slot+nblocks>64){
        // An untouched full segment loaded from disk must be sealed too.
        buffer_dirty=true;
        int ret=cache_sync();
        if(ret<0) return ret;
    }
    if(current_segment<0){
        int ret =new_segment();
        if(ret<0) return ret;
    }
    *first_block=segment_start(current_segment)+next_slot;
    return 0;
}

int cache_append_record(uint32_t op, const char *payload, const int32_t *map,
                        int nblocks, uint64_t first_block){
    if(image_fd<0) return -EBADF;
    if(!payload ||!map ||nblocks<1 ||nblocks>63 ||map[0]!=-2 ||
       op<OP_CREATE ||op>OP_RMDIR) return -EINVAL;
    if(current_segment<0 ||next_slot+nblocks>64 ||
       first_block!=segment_start(current_segment)+next_slot) return -EINVAL;

    // Validate everything before changing the buffer or checkpoint.
    for(int i=0;i<nblocks;i++){
        if(map[i]==-2){
            logfs_inode inode;
            memcpy(&inode, payload+i*4096, sizeof(inode));
            if(inode.ino>=1024) return -EINVAL;
            for(int j=0;j<i;j++){
                if(map[j]!=-2) continue;
                uint64_t other;
                memcpy(&other, payload+j*4096, sizeof(other));
                if(other==inode.ino) return -EINVAL;
            }
            if(checkpoint_cache.imap[inode.ino]){
                logfs_inode old;
                int ret=cache_read_inode(inode.ino, &old);
                if(ret<0) return ret;
            }
        }else if(map[i]<0 ||map[i]>=1024) return -EINVAL;
    }

    char *start=segment_buffer+next_slot*4096;
    memcpy(start, payload, nblocks*4096);
    for(int i=0;i<nblocks;i++)
        if(map[i]==-2) memset(start+i*4096+128, 0, 3968);
    logfs_rec_hdr rec={REC_MAGIC, op, (uint32_t)nblocks, 0};
    memcpy(start+LOGFS_REC_OFFSET, &rec, sizeof(rec));
    rec.crc32 =calc_crc(start, nblocks*4096);
    memcpy(start+LOGFS_REC_OFFSET, &rec, sizeof(rec));

    logfs_segment_hdr hdr;
    memcpy(&hdr, segment_buffer, sizeof(hdr));
    for(int i=0;i<nblocks;i++) hdr.block_map[next_slot+i]=map[i];
    hdr.flags=0;
    hdr.crc32 =calc_crc(&hdr, 268);
    memcpy(segment_buffer, &hdr, sizeof(hdr));

    // Replace inode mappings and invalidate data displaced by COW.
    for(int i=0;i<nblocks;i++){
        if(map[i]!=-2) continue;
        logfs_inode inode;
        memcpy(&inode, start+i*4096, sizeof(inode));
        if(checkpoint_cache.imap[inode.ino]){
            logfs_inode old;
            cache_read_inode(inode.ino, &old); // Already validated above.
            for(int j=0;j<12;j++){
                bool kept=false;
                for(int k=0;k<12;k++)
                    if(old.direct[j]==inode.direct[k]) kept=true;
                if(old.direct[j] &&!kept) cache_invalidate_data(old.direct[j]);
            }
            cache_invalidate_data(checkpoint_cache.imap[inode.ino]);
        }
        cache_invalidate_inode(inode.ino);
        checkpoint_cache.imap[inode.ino]=first_block+i;
        set_bit(checkpoint_cache.inode_bitmap, inode.ino);
        save_inode(&inode, true);
    }
    for(int i=0;i<nblocks;i++)
        if(map[i]>=0) save_data(first_block+i, start+i*4096, true);
    next_slot +=nblocks;
    buffer_dirty =checkpoint_dirty=true;
    // If exactly full, the next prepare (or unmount) flushes it after
    // the caller has finished any extra checkpoint changes for this op.
    return 0;
}
