#include "lfs.h"
#include "cache.h"
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
using namespace std;

static int fs_fd=-1;
const int DIRENTS_PER_BLOCK=4096 /sizeof(logfs_dirent);

static int maybe_gc(){
    int free=cache_free_segments();
    if(free<0) return free;
    if(free<=3 ||(super_cache.num_segments-free)*100>=super_cache.num_segments*80){
        int ret=lfs_gc();
        if(ret<0) return ret;
    }
    return 0;
}

static int prepare_update(int nblocks, uint64_t *first){
    // Keep three free segments for GC relocation.
    if(cache_segment_room()<nblocks &&cache_free_segments()<=3) return -ENOSPC;
    return cache_prepare_record(nblocks, first);
}

static int split_path(const char *path, vector<string> &parts){
    if(!path ||path[0]!='/') return -EINVAL;
    stringstream ss(path);
    string name;
    while(getline(ss, name, '/')){
        if(name.empty()) continue;
        if(name.size()>255) return -ENAMETOOLONG;
        parts.push_back(name);
    }
    return 0;
}

// Find a name and return its direct-block index and slot within that block.
static int find_entry(const logfs_inode &dir, const string &name,
                      logfs_dirent *out, int *block_index, int *slot){
    if(!S_ISDIR(dir.mode)) return -ENOTDIR;
    char block[4096];
    for(int i=0;i<12;i++){
        if(!dir.direct[i]) continue;
        int ret=cache_read_data(dir.direct[i], block);
        if(ret<0) return ret;
        for(int j=0;j<DIRENTS_PER_BLOCK;j++){
            logfs_dirent entry;
            memcpy(&entry, block+j*sizeof(entry), sizeof(entry));
            if(entry.name_len>255) return -EIO;
            if(entry.name_len!=name.size()) continue;
            if(memcmp(entry.name, name.data(), name.size())!=0) continue;
            *out=entry;
            *block_index=i;
            *slot=j;
            return 0;
        }
    }
    return -ENOENT;
}

static int walk_path(const vector<string> &parts, uint64_t *out){
    vector<uint64_t> stack(1, 0);
    for(size_t i=0;i<parts.size();i++){
        logfs_inode dir;
        int ret=cache_read_inode(stack.back(), &dir);
        if(ret<0) return ret;
        if(!S_ISDIR(dir.mode)) return -ENOTDIR;
        if(parts[i]==".") continue;
        if(parts[i]==".."){
            if(stack.size()>1) stack.pop_back();
            continue;
        }
        logfs_dirent entry;
        int index, slot;
        ret=find_entry(dir, parts[i], &entry, &index, &slot);
        if(ret<0) return ret;
        stack.push_back(entry.ino);
    }
    logfs_inode inode;
    int ret=cache_read_inode(stack.back(), &inode);
    if(ret<0) return ret;
    *out=stack.back();
    return 0;
}

int lfs_lookup(const char *path, uint64_t *ino_out){
    if(fs_fd<0) return -EBADF;
    if(!ino_out) return -EINVAL;
    vector<string> parts;
    int ret =split_path(path, parts);
    if(ret<0) return ret;
    uint64_t ino;
    ret=walk_path(parts, &ino);
    if(ret<0) return ret;
    size_t length=strlen(path);
    if(length>1 &&path[length-1]=='/'){
        logfs_inode inode;
        ret=cache_read_inode(ino, &inode);
        if(ret<0) return ret;
        if(!S_ISDIR(inode.mode)) return -ENOTDIR;
    }
    *ino_out=ino;
    return 0;
}

static int parent_for(const char *path, logfs_inode *parent, string &name){
    if(fs_fd<0) return -EBADF;
    vector<string> parts;
    int ret=split_path(path, parts);
    if(ret<0) return ret;
    if(parts.empty()) return -EBUSY;
    name=parts.back();
    if(name=="." ||name=="..") return -EINVAL;
    parts.pop_back();
    uint64_t ino;
    ret=walk_path(parts, &ino);
    if(ret<0) return ret;
    ret=cache_read_inode(ino, parent);
    if(ret<0) return ret;
    return S_ISDIR(parent->mode) ?0 :-ENOTDIR;
}

static int create_node(const char *path, mode_t mode, bool directory){
    int ret=maybe_gc();
    if(ret<0) return ret;
    logfs_inode parent;
    string name;
    ret=parent_for(path, &parent, name);
    if(ret==-EBUSY) return -EEXIST; // Root already exists.
    if(ret<0) return ret;
    logfs_dirent entry;
    int index, slot;
    ret=find_entry(parent, name, &entry, &index, &slot);
    if(ret==0) return -EEXIST;
    if(ret!=-ENOENT) return ret;
    if(!directory &&path[strlen(path)-1]=='/') return -EINVAL;

    int ino=1;
    while(ino<1024 &&get_bit(checkpoint_cache.inode_bitmap, ino)) ino++;
    if(ino==1024) return -ENOSPC;

    char block[4096];
    index=-1;
    slot=-1;
    for(int i=0;i<12 &&index==-1;i++){
        memset(block, 0, sizeof(block));
        if(parent.direct[i]){
            ret=cache_read_data(parent.direct[i], block);
            if(ret<0) return ret;
        }
        for(int j=0;j<DIRENTS_PER_BLOCK;j++){
            memcpy(&entry, block+j*sizeof(entry), sizeof(entry));
            if(entry.name_len==0){
                index=i;
                slot=j;
                break;
            }
        }
    }
    if(index==-1) return -ENOSPC;

    uint64_t first;
    ret=prepare_update(3, &first);
    if(ret<0) return ret;
    logfs_inode child={};
    child.ino=ino;
    child.mode=(directory ?S_IFDIR :S_IFREG) |(mode &07777);
    child.mtime=time(nullptr);
    entry={};
    entry.ino=ino;
    entry.name_len=name.size();
    memcpy(entry.name, name.data(), name.size());
    memcpy(block+slot*sizeof(entry), &entry, sizeof(entry));
    parent.direct[index]=first+1;
    parent.size=max(parent.size, (uint64_t)(index+1)*4096);
    parent.mtime=child.mtime;

    char payload[3*4096]={};
    memcpy(payload, &child, 4096);
    memcpy(payload+4096, block, 4096);
    memcpy(payload+2*4096, &parent, 4096);
    int32_t map[3]={-2, (int32_t)parent.ino, -2};
    return cache_append_record(directory ?OP_MKDIR :OP_CREATE, payload, map, 3, first);
}

int lfs_create(const char *path, mode_t mode){ return create_node(path, mode, false); }
int lfs_mkdir(const char *path, mode_t mode){ return create_node(path, mode, true); }

static int get_inode(const char *path, logfs_inode *inode){
    uint64_t ino;
    int ret=lfs_lookup(path, &ino);
    if(ret<0) return ret;
    return cache_read_inode(ino, inode);
}

int lfs_getattr(const char *path, struct stat *stbuf){
    if(!stbuf) return -EINVAL;
    logfs_inode inode;
    int ret=get_inode(path, &inode);
    if(ret<0) return ret;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino=inode.ino;
    stbuf->st_mode=inode.mode;
    stbuf->st_size=inode.size;
    stbuf->st_mtime=inode.mtime;
    stbuf->st_atime=inode.mtime;
    stbuf->st_ctime=inode.mtime;
    stbuf->st_nlink=S_ISDIR(inode.mode) ?2 :1;
    stbuf->st_uid=getuid();
    stbuf->st_gid=getgid();
    stbuf->st_blksize=4096;
    for(int i=0;i<12;i++) if(inode.direct[i]) stbuf->st_blocks+=8;
    return 0;
}

int lfs_readdir(const char *path, void *buf, lfs_dir_filler_t filler){
    if(!filler) return -EINVAL;
    logfs_inode dir;
    int ret=get_inode(path, &dir);
    if(ret<0) return ret;
    if(!S_ISDIR(dir.mode)) return -ENOTDIR;
    char block[4096];
    for(int i=0;i<12;i++){
        if(!dir.direct[i]) continue;
        ret=cache_read_data(dir.direct[i], block);
        if(ret<0) return ret;
        for(int j=0;j<DIRENTS_PER_BLOCK;j++){
            logfs_dirent entry;
            memcpy(&entry, block+j*sizeof(entry), sizeof(entry));
            if(entry.name_len==0) continue;
            if(entry.name_len>255) return -EIO;
            logfs_inode child;
            ret=cache_read_inode(entry.ino, &child);
            if(ret<0) return ret;
            char name[256]={};
            memcpy(name, entry.name, entry.name_len);
            ret=filler(buf, name, entry.ino, child.mode);
            if(ret<0) return ret;
            if(ret>0) return 0; // The caller's output buffer is full.
        }
    }
    return 0;
}

int lfs_read(const char *path, char *buf, size_t size, off_t offset){
    if(offset<0 ||(!buf &&size)) return -EINVAL;
    logfs_inode inode;
    int ret=get_inode(path, &inode);
    if(ret<0) return ret;
    if(S_ISDIR(inode.mode)) return -EISDIR;
    if(inode.size>12*4096) return -EIO;
    if((uint64_t)offset>=inode.size ||size==0) return 0;
    size=min(size, (size_t)(inode.size-offset));
    size_t done=0;
    char block[4096];
    while(done<size){
        uint64_t pos=offset+done;
        int index=pos/4096, start=pos%4096;
        size_t count=min(size-done, (size_t)(4096-start));
        memset(block, 0, sizeof(block));
        if(inode.direct[index]){
            ret=cache_read_data(inode.direct[index], block);
            if(ret<0) return ret;
        }
        memcpy(buf+done, block+start, count);
        done+=count;
    }
    return done;
}

int lfs_write(const char *path, const char *buf, size_t size, off_t offset){
    if(offset<0 ||(!buf &&size)) return -EINVAL;
    int ret=maybe_gc();
    if(ret<0) return ret;
    logfs_inode inode;
    ret=get_inode(path, &inode);
    if(ret<0) return ret;
    if(S_ISDIR(inode.mode)) return -EISDIR;
    if(size==0) return 0;
    if(offset>12*4096 ||size>(size_t)(12*4096-offset)) return -EFBIG;
    int begin=offset/4096, end=(offset+size-1)/4096;
    int nblocks=end-begin+2;
    vector<char> payload(nblocks*4096, 0);
    vector<int32_t> map(nblocks, inode.ino);
    map[0]=-2;
    uint64_t first;
    ret=prepare_update(nblocks, &first);
    if(ret<0) return ret;
    size_t done=0;
    for(int i=begin;i<=end;i++){
        int slot=i-begin+1;
        char *block=payload.data()+slot*4096;
        if(inode.direct[i]){
            ret=cache_read_data(inode.direct[i], block);
            if(ret<0) return ret;
        }
        int start=(i==begin) ?offset%4096 :0;
        size_t count=min(size-done, (size_t)(4096-start));
        memcpy(block+start, buf+done, count);
        done+=count;
        inode.direct[i]=first+slot;
    }
    inode.size=max(inode.size, (uint64_t)offset+size);
    inode.mtime=time(nullptr);
    memcpy(payload.data(), &inode, 4096);
    ret=cache_append_record(OP_WRITE, payload.data(), map.data(), nblocks, first);
    return ret<0 ?ret :(int)size;
}

int lfs_rmdir(const char *path){
    int ret=maybe_gc();
    if(ret<0) return ret;
    logfs_inode parent;
    string name;
    ret=parent_for(path, &parent, name);
    if(ret<0) return ret;
    logfs_dirent entry;
    int index, slot;
    ret=find_entry(parent, name, &entry, &index, &slot);
    if(ret<0) return ret;
    logfs_inode child;
    ret=cache_read_inode(entry.ino, &child);
    if(ret<0) return ret;
    if(!S_ISDIR(child.mode)) return -ENOTDIR;
    char block[4096];
    for(int i=0;i<12;i++){
        if(!child.direct[i]) continue;
        ret=cache_read_data(child.direct[i], block);
        if(ret<0) return ret;
        for(int j=0;j<DIRENTS_PER_BLOCK;j++){
            logfs_dirent item;
            memcpy(&item, block+j*sizeof(item), sizeof(item));
            if(item.name_len) return -ENOTEMPTY;
        }
    }
    ret=cache_read_data(parent.direct[index], block);
    if(ret<0) return ret;
    memset(block+slot*sizeof(entry), 0, sizeof(entry));
    uint64_t first;
    ret=prepare_update(2, &first);
    if(ret<0) return ret;
    parent.direct[index]=first+1;
    parent.mtime=time(nullptr);
    char payload[2*4096]={};
    memcpy(payload, &parent, 4096);
    memcpy(payload+4096, block, 4096);
    int32_t map[2]={-2, (int32_t)parent.ino};
    ret=cache_append_record(OP_RMDIR, payload, map, 2, first);
    if(ret<0) return ret;
    checkpoint_cache.imap[child.ino]=0;
    clear_bit(checkpoint_cache.inode_bitmap, child.ino);
    cache_checkpoint_changed();
    cache_invalidate_inode(child.ino);
    for(int i=0;i<12;i++)
        if(child.direct[i]) cache_invalidate_data(child.direct[i]);
    return 0;
}

int lfs_mount(const char *img_path){
    if(fs_fd>=0) return -EBUSY;
    if(!img_path) return -EINVAL;
    int fd=open(img_path, O_RDWR);
    if(fd<0) return -errno;
    int ret=cache_init(fd);
    uint64_t first;
    if(ret==0) ret=maybe_gc();
    if(ret==0) ret=prepare_update(1, &first);
    if(ret<0){
        cache_reset();
        close(fd);
        return ret;
    }
    fs_fd=fd;
    return 0;
}

int lfs_umount(void){
    if(fs_fd<0) return -EBADF;
    int ret=cache_sync();
    if(ret<0) return ret; // Keep memory and fd so the caller can retry.
    int fd=fs_fd;
    fs_fd=-1;
    cache_reset();
    if(close(fd)<0) return -errno;
    return 0;
}
