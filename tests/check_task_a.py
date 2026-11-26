import os
import stat
import struct
import subprocess
import tempfile
import zlib


def crc(data):
    return zlib.crc32(data) & 0xffffffff


with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "logfs.img")
    subprocess.run(["./mkfs.logfs", path], check=True)
    assert os.path.getsize(path) == 256 * 1024 * 1024

    with open(path, "rb") as f:
        sb = f.read(4096)
        cp = f.read(12288)
        seg = f.read(4096)
        root = f.read(4096)
        rest = f.read(62 * 4096)

    assert struct.unpack_from("<6I3Q", sb) == (
        0x4c4f4746, 1, 4096, 64, 1024, 1023, 65536, 1, 4
    )
    assert crc(sb[:48]) == struct.unpack_from("<I", sb, 48)[0]
    assert sb[52:] == bytes(4044)

    imap = struct.unpack_from("<1024Q", cp)
    assert imap[0] == 5 and all(x == 0 for x in imap[1:])
    assert cp[8192:9216] == b"\x01" + bytes(1023)
    assert cp[9216:10240] == b"\x01" + bytes(1023)
    assert crc(cp[:10240]) == struct.unpack_from("<I", cp, 10240)[0]
    assert cp[10244:] == bytes(2044)

    assert struct.unpack_from("<3I", seg) == (0x5345474d, 1, 0)
    block_map = struct.unpack_from("<64i", seg, 12)
    assert block_map[1] == -2
    assert all(x == -1 for i, x in enumerate(block_map) if i != 1)
    assert crc(seg[:268]) == struct.unpack_from("<I", seg, 268)[0]
    assert seg[272:] == bytes(3824)
    assert rest == bytes(62 * 4096)

    ino, mode, reserved, size, mtime = struct.unpack_from("<QIIQq", root)
    assert ino == 0 and mode == stat.S_IFDIR | 0o755
    assert reserved == 0 and size == 0 and mtime > 0
    assert struct.unpack_from("<12Q", root, 32) == (0,) * 12
    assert root[128:4080] == bytes(3952)
    magic, op, nblocks, checksum = struct.unpack_from("<4I", root, 4080)
    assert (magic, op, nblocks) == (0x52454352, 2, 1)
    assert crc(root[:4092] + bytes(4)) == checksum

    # The initial segment ends at block 67; unused image space is zero.
    with open(path, "rb") as f:
        f.seek(68 * 4096)
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            assert not any(chunk)

    # An existing image should be refused without changing its contents.
    result = subprocess.run(["./mkfs.logfs", path], capture_output=True)
    assert result.returncode != 0
    with open(path, "rb") as f:
        assert f.read(4096) == sb
    assert os.path.getsize(path) == 256 * 1024 * 1024

    result = subprocess.run(["./mkfs.logfs", folder + "/missing/logfs.img"],
                            capture_output=True)
    assert result.returncode != 0

print("Task A checks passed")
