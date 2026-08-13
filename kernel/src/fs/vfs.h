#ifndef FS_VFS_H
#define FS_VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX 64

enum vnode_type {
    VNODE_DIR,
    VNODE_FILE,
};

struct vnode {
    char name[VFS_NAME_MAX];
    enum vnode_type type;
    struct vnode *parent;
    struct vnode *children; /* First child (dir only). */
    struct vnode *sibling;  /* Next sibling within the parent's children list. */
    uint8_t *data;          /* File content (file only), kmalloc'd. */
    size_t size;
    size_t capacity;
};

/* In-memory-only filesystem: everything here lives in kernel heap memory
 * (see mm/heap.h) and resets on reboot -- there's no block device or
 * on-disk format yet. Every function reports its own error via kprintf
 * and returns 0 on success / -1 on failure, matching this codebase's
 * existing convention (see drivers/virtio/virtio.c) rather than a rich
 * error-code API. */

void vfs_init(void);
struct vnode *vfs_root(void);

/* Resolves an absolute ("/a/b") or relative (to `base`) path, handling
 * "." and "..". Returns NULL (no message -- "not found" is a normal,
 * expected outcome callers handle themselves) if any component doesn't
 * exist, or a non-leaf component isn't a directory. */
struct vnode *vfs_resolve(struct vnode *base, const char *path);

/* Both resolve `path`'s parent (which must already exist -- no `-p`
 * style recursive creation) and create the leaf. Fail if the leaf
 * already exists. */
int vfs_mkdir(struct vnode *base, const char *path);
int vfs_create_file(struct vnode *base, const char *path);

/* Removes a file unconditionally, or a directory and everything under it
 * recursively. Refuses (and reports why) if the target is the root, or
 * is `cwd` or an ancestor of `cwd` -- removing those would leave the
 * caller holding a dangling pointer. */
int vfs_remove(struct vnode *base, const char *path, struct vnode *cwd);

/* If `dest_path` resolves to an existing directory, the source lands
 * inside it under its own name (cp/mv-into-directory convention);
 * otherwise `dest_path` is the full new parent+name. Both refuse if the
 * final destination name already exists (no silent overwrite/merge) or,
 * for directories, if the destination is the source itself or one of
 * its own descendants. */
int vfs_copy(struct vnode *base, const char *src_path, const char *dest_path);
int vfs_move(struct vnode *base, const char *src_path, const char *dest_path);

/* Overwrites (or appends to, if `append`) a file's content, creating it
 * first if it doesn't exist yet. */
int vfs_write_file(struct vnode *base, const char *path, const char *text, int append);

/* Like vfs_write_file() with append=0, but takes an explicit byte length
 * instead of relying on strlen() -- for restoring content that isn't
 * necessarily NUL-terminated text (see fs/blkfs.c). Creates the file
 * first if it doesn't exist yet. */
int vfs_write_bytes(struct vnode *base, const char *path, const void *data, size_t size);

/* Prints a file's content via kprintf, followed by one newline. */
int vfs_cat(struct vnode *base, const char *path);

/* Lists `path`'s children (or `base`'s, if path is NULL/empty) via
 * kprintf, one per line, directories suffixed with '/'. */
void vfs_list(struct vnode *base, const char *path);

/* Reconstructs the full path to `node` by walking parent links. */
void vfs_path(struct vnode *node, char *out, size_t out_size);

#endif
