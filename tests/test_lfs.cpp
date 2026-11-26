#include "lfs.h"
#include "cache.h"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
using namespace std;

void check(int got, int expected, const char *name){
    if(got==expected) return;
    cerr<<name<<": expected " <<expected<<", got "<<got<<"\n";
    exit(1);
}

int collect(void *buf, const char *name, uint64_t ino, mode_t mode){
    (void)ino;
    (void)mode;
    vector<string> *names=(vector<string> *)buf;
    names->push_back(name);
    return 0;
}

int stop_after_one(void *buf, const char *name, uint64_t ino, mode_t mode){
    collect(buf, name, ino, mode);
    return 1;
}

int main(int argc, char **argv){
    if(argc>2){
        cerr<<"Usage: ./test_lfs [fresh_image]\n";
        return 1;
    }
    string image;
    char folder[]="/tmp/logfs-test-XXXXXX";
    bool temporary=(argc==1);
    if(temporary){
        if(!mkdtemp(folder)) return 1;
        image=string(folder)+"/logfs.img";
        pid_t pid=fork();
        if(pid<0) return 1;
        if(pid==0){
            execl("./mkfs.logfs", "mkfs.logfs", image.c_str(), (char *)nullptr);
            _exit(1);
        }
        int status;
        if(waitpid(pid, &status, 0)<0 ||!WIFEXITED(status) ||WEXITSTATUS(status)!=0)
            return 1;
    }else image=argv[1];

    check(lfs_mount(image.c_str()), 0, "mount");
    check(lfs_mount(image.c_str()), -EBUSY, "duplicate mount");
    struct stat st;
    check(lfs_getattr("/", &st), 0, "root getattr");
    check(S_ISDIR(st.st_mode), 1, "root directory");
    check(lfs_mkdir("/docs", 0755), 0, "mkdir");
    check(lfs_mkdir("/docs/sub", 0750), 0, "nested mkdir");
    check(lfs_mkdir("/docs", 0755), -EEXIST, "duplicate mkdir");
    check(lfs_mkdir("/", 0755), -EEXIST, "root already exists");
    check(lfs_create("/docs/a.txt", 0644), 0, "create");
    check(lfs_create("/docs/a.txt", 0644), -EEXIST, "duplicate create");
    check(lfs_create("/missing/a", 0644), -ENOENT, "missing parent");
    check(lfs_create("/docs/a.txt/child", 0644), -ENOTDIR, "file parent");

    vector<char> expected(12*4096), got(12*4096);
    for(size_t i=0;i<expected.size();i++) expected[i]=i%251;
    check(lfs_write("/docs/a.txt", expected.data(), expected.size(), 0),
          expected.size(), "full file write");
    check(lfs_write("/docs/a.txt", "abcdefghijklmnopqrst", 20, 4090),
          20, "cross-block overwrite");
    memcpy(expected.data()+4090, "abcdefghijklmnopqrst", 20);
    check(lfs_read("/docs/a.txt", got.data(), got.size(), 0), got.size(), "read");
    check(memcmp(expected.data(), got.data(), got.size()), 0, "file contents");
    check(lfs_read("/docs/a.txt", got.data(), 10, 49152), 0, "EOF");
    check(lfs_write("/docs/a.txt", "x", 1, 49152), -EFBIG, "file limit");
    check(lfs_write("/docs/a.txt", "x", 1, -1), -EINVAL, "negative offset");
    check(lfs_read("/docs", got.data(), 10, 0), -EISDIR, "read directory");
    check(lfs_write("/docs", "x", 1, 0), -EISDIR, "write directory");

    check(lfs_create("/docs/hole", 0600), 0, "sparse file");
    check(lfs_write("/docs/hole", "hello", 5, 9000), 5, "write after hole");
    memset(got.data(), 1, got.size());
    check(lfs_read("/docs/hole", got.data(), 10000, 0), 9005, "sparse read");
    for(int i=0;i<9000;i++) check(got[i], 0, "zero-filled hole");
    check(memcmp(got.data()+9000, "hello", 5), 0, "sparse contents");
    check(lfs_getattr("/docs/hole", &st), 0, "getattr");
    check(st.st_size, 9005, "file size");
    check(st.st_mode &0777, 0600, "file mode");

    vector<string> names;
    check(lfs_readdir("/docs", &names, collect), 0, "readdir");
    sort(names.begin(), names.end());
    check(names==vector<string>({"a.txt", "hole", "sub"}), 1, "directory names");
    names.clear();
    check(lfs_readdir("/docs", &names, stop_after_one), 0, "full output buffer");
    check(names.size(), 1, "one entry");
    check(lfs_readdir("/docs/a.txt", &names, collect), -ENOTDIR, "readdir file");
    check(lfs_rmdir("/docs"), -ENOTEMPTY, "nonempty rmdir");
    check(lfs_rmdir("/docs/a.txt"), -ENOTDIR, "rmdir file");
    check(lfs_rmdir("/"), -EBUSY, "rmdir root");
    uint64_t old_ino, new_ino;
    check(lfs_lookup("/docs/sub", &old_ino), 0, "lookup");
    check(lfs_rmdir("/docs/sub"), 0, "empty rmdir");
    check(lfs_lookup("/docs/sub", &new_ino), -ENOENT, "removed directory");
    check(lfs_mkdir("/again", 0755), 0, "reuse inode");
    check(lfs_lookup("/again", &new_ino), 0, "reused inode lookup");
    check(old_ino==new_ino, 1, "same inode number reused");
    check(lfs_rmdir("/again"), 0, "remove reused inode");
    check(lfs_mkdir("/empty_parent", 0755), 0, "parent mkdir");
    check(lfs_mkdir("/empty_parent/child", 0755), 0, "child mkdir");
    check(lfs_rmdir("/empty_parent/child"), 0, "child rmdir");
    check(lfs_rmdir("/empty_parent"), 0, "empty allocated directory rmdir");

    string long_name="/"+string(255, 'x');
    check(lfs_create(long_name.c_str(), 0644), 0, "255-byte name");
    long_name+='x';
    check(lfs_create(long_name.c_str(), 0644), -ENAMETOOLONG, "name limit");
    check(lfs_lookup("relative", &new_ino), -EINVAL, "absolute path required");
    check(lfs_lookup("/docs/../docs/./a.txt", &new_ino), 0, "dot paths");
    check(lfs_lookup("/docs/a.txt/", &new_ino), -ENOTDIR, "file trailing slash");

    // More than 64 files exercise eviction and several segment rollovers.
    for(int i=0;i<80;i++){
        string path="/docs/f"+to_string(i);
        string text="value"+to_string(i);
        check(lfs_create(path.c_str(), 0644), 0, "many creates");
        check(lfs_write(path.c_str(), text.data(), text.size(), 0), text.size(), "many writes");
    }
    check(lfs_umount(), 0, "unmount");
    check(lfs_umount(), -EBADF, "duplicate unmount");
    check(lfs_mount(image.c_str()), 0, "remount");
    check(lfs_read("/docs/a.txt", got.data(), got.size(), 0), got.size(), "read after remount");
    check(memcmp(expected.data(), got.data(), got.size()), 0, "persistent contents");
    for(int i=0;i<80;i++){
        string path="/docs/f"+to_string(i);
        string text="value"+to_string(i);
        char buf[32]={};
        check(lfs_read(path.c_str(), buf, sizeof(buf), 0), text.size(), "many persistent reads");
        check(memcmp(buf, text.data(), text.size()), 0, "many file contents");
    }

    // Keep live blocks beside repeatedly overwritten (dead) blocks.
    check(lfs_create("/docs/keep", 0644), 0, "GC keep create");
    vector<char> keep(8192, 'k');
    check(lfs_write("/docs/keep", keep.data(), keep.size(), 0), keep.size(), "GC keep write");
    check(lfs_create("/docs/hot", 0644), 0, "GC hot create");
    vector<char> hot(12*4096);
    for(int i=0;i<32;i++){
        fill(hot.begin(), hot.end(), 'a'+i%26);
        check(lfs_write("/docs/hot", hot.data(), hot.size(), 0), hot.size(), "GC overwrites");
    }
    uint64_t keep_ino;
    check(lfs_lookup("/docs/keep", &keep_ino), 0, "GC keep lookup");
    logfs_inode before_keep;
    check(cache_read_inode(keep_ino, &before_keep), 0, "GC keep inode");
    uint64_t old_keep_inode=checkpoint_cache.imap[keep_ino];
    int free_before=cache_free_segments();
    unsigned char old_bitmap[1024];
    memcpy(old_bitmap, checkpoint_cache.segment_bitmap, 1024);
    vector<string> before_names;
    check(lfs_readdir("/docs", &before_names, collect), 0, "GC directory before");
    sort(before_names.begin(), before_names.end());

    int total_cleaned=0;
    for(int i=0;i<1024;i++){
        int ret=lfs_gc();
        check(ret>=0, 1, "GC result");
        if(ret==0) break;
        total_cleaned+=ret;
    }
    check(total_cleaned>0, 1, "segments cleaned");
    check(cache_free_segments()>free_before, 1, "space reclaimed");
    logfs_inode after_keep;
    check(cache_read_inode(keep_ino, &after_keep), 0, "GC moved inode");
    check(checkpoint_cache.imap[keep_ino]!=old_keep_inode, 1, "live inode relocated");
    check(after_keep.direct[0]!=before_keep.direct[0], 1, "live data relocated");
    check(lfs_read("/docs/keep", got.data(), got.size(), 0), keep.size(), "GC keep read");
    check(memcmp(got.data(), keep.data(), keep.size()), 0, "GC keep contents");
    check(lfs_read("/docs/hot", got.data(), got.size(), 0), hot.size(), "GC hot read");
    check(memcmp(got.data(), hot.data(), hot.size()), 0, "GC latest contents");
    check(lfs_read("/docs/a.txt", got.data(), got.size(), 0), expected.size(), "GC old file read");
    check(memcmp(got.data(), expected.data(), expected.size()), 0, "GC old file contents");
    vector<string> after_names;
    check(lfs_readdir("/docs", &after_names, collect), 0, "GC directory after");
    sort(after_names.begin(), after_names.end());
    check(before_names==after_names, 1, "GC directory preserved");
    for(int i=0;i<80;i++){
        string path="/docs/f"+to_string(i);
        string text="value"+to_string(i);
        char buf[32]={};
        check(lfs_read(path.c_str(), buf, sizeof(buf), 0), text.size(), "GC existing file read");
        check(memcmp(buf, text.data(), text.size()), 0, "GC existing file contents");
    }

    int reused=-1;
    for(int i=0;i<(int)super_cache.num_segments;i++){
        if(get_bit(old_bitmap, i) &&!get_bit(checkpoint_cache.segment_bitmap, i)){
            reused=i;
            break;
        }
    }
    check(reused>=0, 1, "freed segment found");
    check(lfs_create("/reused", 0644), 0, "create after GC");
    uint64_t reused_ino;
    check(lfs_lookup("/reused", &reused_ino), 0, "reused lookup");
    int new_segment=(checkpoint_cache.imap[reused_ino]-super_cache.log_start_block)/64;
    check(new_segment, reused, "freed segment reused");
    check(lfs_lookup("/docs/sub", &new_ino), -ENOENT, "persistent removal");
    check(lfs_mkdir("/after_remount", 0755), 0, "append after remount");
    check(lfs_umount(), 0, "final unmount");
    int fd=open(image.c_str(), O_RDWR);
    check(fd>=0, 1, "open for checksum check");
    char original, bad;
    check(pread(fd, &original, 1, 0), 1, "read superblock byte");
    bad=original ^1;
    check(pwrite(fd, &bad, 1, 0), 1, "corrupt checksum");
    check(lfs_mount(image.c_str()), -EIO, "reject bad checksum");
    check(pwrite(fd, &original, 1, 0), 1, "restore byte");
    close(fd);
    check(lfs_mount(image.c_str()), 0, "mount after failed mount");
    check(lfs_read("/docs/keep", got.data(), got.size(), 0), keep.size(), "GC remount read");
    check(memcmp(got.data(), keep.data(), keep.size()), 0, "GC remount contents");
    check(lfs_umount(), 0, "unmount restored image");
    check(lfs_gc(), -EBADF, "GC without mount");
    if(temporary){
        unlink(image.c_str());
        rmdir(folder);
    }
    cout<<"Phase 1 Tasks C-D checks passed\n";
}
