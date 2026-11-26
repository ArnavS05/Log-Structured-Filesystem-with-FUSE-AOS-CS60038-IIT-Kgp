#include "lfs.h"
#include "cache.h"
#include <vector>
#include <cstring>
#include <cerrno>
using namespace std;

static uint64_t segment_start(int segment){
    return super_cache.log_start_block+segment*64ULL;
}

static bool in_segment(uint64_t block, int segment){
    uint64_t start=segment_start(segment);
    return block>start &&block<start+64;
}

// Mark the inodes that own live blocks in this segment.
static int count_live(int segment, const logfs_segment_hdr &hdr,
                      int *live, int *total, bool *affected){
    *live=*total=0;
    memset(affected, 0, 1024*sizeof(bool));
    uint64_t start=segment_start(segment);
    for(int i=1;i<64;i++){
        int owner=hdr.block_map[i];
        if(owner==-1) continue;
        (*total)++;
        uint64_t block=start+i;
        if(owner==-2){
            for(int ino=0;ino<1024;ino++){
                if(checkpoint_cache.imap[ino]!=block) continue;
                affected[ino]=true;
                (*live)++;
                break;
            }
        }else{
            if(owner<0 ||owner>=1024) return -EIO;
            if(!checkpoint_cache.imap[owner]) continue;
            logfs_inode inode;
            int ret=cache_read_inode(owner, &inode);
            if(ret<0) return ret;
            for(int j=0;j<12;j++){
                if(inode.direct[j]!=block) continue;
                affected[owner]=true;
                (*live)++;
                break;
            }
        }
    }
    return 0;
}

// Simulate record packing so relocation does not run out of free segments.
static int space_needed(int segment, const bool *affected){
    int room=cache_segment_room(), needed=0;
    for(int ino=0;ino<1024;ino++){
        if(!affected[ino]) continue;
        logfs_inode inode;
        int ret=cache_read_inode(ino, &inode);
        if(ret<0) return ret;
        int count=1; // New inode, even if only its data is being moved.
        for(int j=0;j<12;j++)
            if(in_segment(inode.direct[j], segment)) count++;
        if(count>room){
            needed++;
            room=63;
        }
        room-=count;
    }
    return needed;
}

static int move_live(int segment, const bool *affected){
    for(int ino=0;ino<1024;ino++){
        if(!affected[ino]) continue;
        logfs_inode inode;
        int ret=cache_read_inode(ino, &inode);
        if(ret<0) return ret;
        int count=1;
        for(int j=0;j<12;j++)
            if(in_segment(inode.direct[j], segment)) count++;

        vector<char> payload(count*4096, 0);
        vector<int32_t> map(count, ino);
        map[0]=-2;
        uint64_t first;
        ret=cache_prepare_record(count, &first);
        if(ret<0) return ret;
        int slot=1;
        for(int j=0;j<12;j++){
            if(!in_segment(inode.direct[j], segment)) continue;
            ret=cache_read_data(inode.direct[j], payload.data()+slot*4096);
            if(ret<0) return ret;
            inode.direct[j]=first+slot;
            slot++;
        }
        memcpy(payload.data(), &inode, 4096);
        // Relocation uses WRITE records for file and directory blocks.
        // File size, mode and mtime are preserved.
        ret=cache_append_record(OP_WRITE, payload.data(), map.data(), count, first);
        if(ret<0) return ret;
    }
    return 0;
}

int lfs_gc(void){
    int free=cache_free_segments();
    if(free<0) return free;
    bool high=(super_cache.num_segments-free)*100 >=super_cache.num_segments*80;
    bool candidate[1024]={};
    vector<int> cleaned;
    logfs_segment_hdr hdr;

    // Only segments sealed before this call are candidates.
    for(uint32_t s=0;s<super_cache.num_segments;s++){
        if(!get_bit(checkpoint_cache.segment_bitmap, s)) continue;
        int ret=cache_read_segment_header(s, &hdr);
        if(ret<0) return ret;
        candidate[s]=(hdr.flags &SEG_SEALED)!=0 &&(int)s!=cache_current_segment();
    }

    // Bounded work; pack survivors from up to four victims together.
    for(int round=0;round<4;round++){
        int best=-1, best_live=0, best_total=1;
        bool affected[1024];
        for(uint32_t s=0;s<super_cache.num_segments;s++){
            if(!candidate[s]) continue;
            int ret=cache_read_segment_header(s, &hdr);
            if(ret<0) return ret;
            int live, total;
            ret=count_live(s, hdr, &live, &total, affected);
            if(ret<0) return ret;
            if(total &&live==total) continue; // Moving only live blocks gains nothing.
            if(total &&live*4>=total &&!high) continue;
            int denominator=total ?total :1;
            if(best==-1 ||live*best_total<best_live*denominator){
                best=s;
                best_live=live;
                best_total=denominator;
            }
        }
        if(best==-1) break;
        int ret=cache_read_segment_header(best, &hdr);
        if(ret<0) return ret;
        int live, total;
        ret=count_live(best, hdr, &live, &total, affected);
        if(ret<0) return ret;
        int needed=space_needed(best, affected);
        if(needed<0) return needed;
        if(needed>cache_free_segments()){
            if(cleaned.empty()) return -ENOSPC;
            break;
        }
        ret=move_live(best, affected);
        if(ret<0) return ret;
        cleaned.push_back(best);
        candidate[best]=false;
    }
    if(cleaned.empty()) return 0;

    // Persist new blocks and pointers before making old segments reusable.
    int ret=cache_sync();
    if(ret<0) return ret;
    for(size_t i=0;i<cleaned.size();i++){
        clear_bit(checkpoint_cache.segment_bitmap, cleaned[i]);
        for(int j=1;j<64;j++)
            cache_invalidate_data(segment_start(cleaned[i])+j);
    }
    cache_checkpoint_changed();
    ret=cache_sync();
    return ret<0 ?ret :(int)cleaned.size();
}
