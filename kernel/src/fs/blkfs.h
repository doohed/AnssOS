#ifndef FS_BLKFS_H
#define FS_BLKFS_H

/* Persists the in-memory VFS tree (fs/vfs.h) to/from the virtio-blk
 * device, in a simple custom format -- not a "real" filesystem (no
 * inode table, no free-space bitmap, no incremental updates): every
 * save serializes the WHOLE tree and overwrites the disk image in one
 * go. Fine at hobby-OS scale; would need a real design to scale up.
 * drivers/virtio/virtio_blk.h's virtio_blk_init() must have already
 * succeeded, or both functions just report "no disk" and return 0. */

/* Reads the superblock; if it has a valid magic, deserializes the
 * stored tree by replaying vfs_mkdir()/vfs_create_file()/
 * vfs_write_bytes() calls against vfs_root(). Returns 0 on success,
 * including the "blank disk, nothing to load yet" case (not an error --
 * vfs_init()'s fresh empty root just stands as-is). Returns -1 only on
 * a real I/O error or corrupt on-disk data. */
int blkfs_load(void);

/* Serializes the entire current in-memory tree and writes it (plus a
 * fresh superblock) to the virtio-blk device, overwriting whatever was
 * there. Returns 0 on success (or if there's no virtio-blk device at
 * all -- a silent no-op, not an error), -1 on failure (including "too
 * big for the disk"). */
int blkfs_save(void);

#endif
