#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "lfs.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
using namespace std;

static bool mounted=false;
static int close_error=0;

static int logfs_getattr(const char *path, struct stat *st,
                         struct fuse_file_info *){
    return lfs_getattr(path, st);
}

struct dir_buffer {
    void *buf;
    fuse_fill_dir_t filler;
    off_t skip, index;
};

static int fill_entry(void *buf, const char *name, uint64_t ino, mode_t mode){
    dir_buffer *dir=(dir_buffer *)buf;
    dir->index++;
    if(dir->index<=dir->skip) return 0;
    struct stat st={};
    st.st_ino=ino;
    st.st_mode=mode;
    // The next call resumes after this entry if FUSE's buffer fills up.
    return dir->filler(dir->buf, name, &st, dir->index,
                       (enum fuse_fill_dir_flags)0);
}

static int logfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *,
                         enum fuse_readdir_flags){
    if(offset<0) return -EINVAL;
    struct stat st;
    int ret=lfs_getattr(path, &st);
    if(ret<0) return ret;
    if(!S_ISDIR(st.st_mode)) return -ENOTDIR;
    uint64_t parent;
    string parent_path=string(path)+"/..";
    ret=lfs_lookup(parent_path.c_str(), &parent);
    if(ret<0) return ret;
    dir_buffer dir={buf, filler, offset, 0};
    if(fill_entry(&dir, ".", st.st_ino, st.st_mode)) return 0;
    if(fill_entry(&dir, "..", parent, S_IFDIR)) return 0;
    return lfs_readdir(path, &dir, fill_entry);
}

static int logfs_open(const char *path, struct fuse_file_info *fi){
    struct stat st;
    int ret=lfs_getattr(path, &st);
    if(ret<0) return ret;
    if(S_ISDIR(st.st_mode)) return -EISDIR;
    if((fi->flags &O_TRUNC) &&st.st_size) return -EOPNOTSUPP;
    fi->direct_io=1; // Reads and writes go through our library's caches.
    return 0;
}

static int logfs_create(const char *path, mode_t mode, struct fuse_file_info *fi){
    int ret=lfs_create(path, mode);
    if(ret==0) fi->direct_io=1;
    return ret;
}

static int logfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *){
    return lfs_read(path, buf, size, offset);
}

static int logfs_write(const char *path, const char *buf, size_t size, off_t offset,
                       struct fuse_file_info *){
    return lfs_write(path, buf, size, offset);
}

static int logfs_mkdir(const char *path, mode_t mode){ return lfs_mkdir(path, mode); }
static int logfs_rmdir(const char *path){ return lfs_rmdir(path); }

// Task C has no truncate API. Only a request that changes nothing is accepted.
static int logfs_truncate(const char *path, off_t size, struct fuse_file_info *){
    if(size<0) return -EINVAL;
    struct stat st;
    int ret=lfs_getattr(path, &st);
    if(ret<0) return ret;
    if(S_ISDIR(st.st_mode)) return -EISDIR;
    return size==st.st_size ?0 :-EOPNOTSUPP;
}

static void *logfs_init(struct fuse_conn_info *, struct fuse_config *cfg){
    cfg->kernel_cache=0;
    cfg->attr_timeout=0;
    cfg->entry_timeout=0;
    cfg->negative_timeout=0;
    return nullptr;
}

static void logfs_destroy(void *){
    if(!mounted) return;
    close_error=lfs_umount();
    if(close_error<0) cerr<<"unmount: "<<strerror(-close_error)<<"\n";
    else mounted=false;
}

int main(int argc, char **argv){
    if(argc<2){
        cerr<<"usage: ./mount.logfs [-f] <mountpoint>\n";
        return 1;
    }
    bool help=false;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i], "-h") ||!strcmp(argv[i], "--help") ||
           !strcmp(argv[i], "-V") ||!strcmp(argv[i], "--version")) help=true;
    }
    // Open before fuse_main: background mode can change the working directory.
    if(!help){
        int ret=lfs_mount("logfs.img");
        if(ret<0){
            cerr<<"logfs.img: "<<strerror(-ret)<<"\n";
            return 1;
        }
        mounted=true;
    }

    struct fuse_operations ops={};
    ops.getattr=logfs_getattr;
    ops.readdir=logfs_readdir;
    ops.open=logfs_open;
    ops.create=logfs_create;
    ops.read=logfs_read;
    ops.write=logfs_write;
    ops.mkdir=logfs_mkdir;
    ops.rmdir=logfs_rmdir;
    ops.truncate=logfs_truncate;
    ops.init=logfs_init;
    ops.destroy=logfs_destroy;

    struct fuse_args args=FUSE_ARGS_INIT(0, nullptr);
    for(int i=0;i<argc;i++){
        if(fuse_opt_add_arg(&args, argv[i])<0){
            fuse_opt_free_args(&args);
            logfs_destroy(nullptr);
            return 1;
        }
    }
    // Shared caches are single-threaded. Let the kernel check mode permissions.
    if(fuse_opt_add_arg(&args, "-s")<0 ||fuse_opt_add_arg(&args, "-o")<0 ||
       fuse_opt_add_arg(&args, "default_permissions")<0){
        fuse_opt_free_args(&args);
        logfs_destroy(nullptr);
        return 1;
    }
    int ret=fuse_main(args.argc, args.argv, &ops, nullptr);
    fuse_opt_free_args(&args);
    if(mounted) logfs_destroy(nullptr); // Also close after a failed FUSE startup.
    return close_error<0 ?1 :ret;
}
