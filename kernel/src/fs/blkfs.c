#include "blkfs.h"
#include "vfs.h"
#include "../drivers/serial.h"
#include "../drivers/virtio/virtio_blk.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

#define BLKFS_MAGIC 0x53464e41u /* "ANFS" (AnssOS FS), read as a little-endian u32. */
#define BLKFS_VERSION 1

struct __attribute__((packed)) blkfs_superblock {
    uint32_t magic;
    uint32_t version;
    uint64_t total_size; /* Bytes of serialized tree data, starting at sector 1. */
};

/* --- Serialization: a growable cursor over a buffer, or over nothing at
 * all (buf == NULL) to just measure how many bytes a walk would take. --- */

struct writer {
    uint8_t *buf;
    uint64_t pos;
    uint64_t cap;
};

static int w_bytes(struct writer *w, const void *data, uint64_t len) {
    if (w->buf != NULL) {
        if (w->pos + len > w->cap) {
            return -1;
        }
        memcpy(w->buf + w->pos, data, len);
    }
    w->pos += len;
    return 0;
}

static int w_u8(struct writer *w, uint8_t v) {
    return w_bytes(w, &v, sizeof(v));
}

static int w_u32(struct writer *w, uint32_t v) {
    return w_bytes(w, &v, sizeof(v));
}

static int serialize_node(struct writer *w, struct vnode *node) {
    if (w_u8(w, (uint8_t)node->type) != 0) {
        return -1;
    }
    uint8_t name_len = (uint8_t)strlen(node->name);
    if (w_u8(w, name_len) != 0 || w_bytes(w, node->name, name_len) != 0) {
        return -1;
    }

    if (node->type == VNODE_FILE) {
        if (w_u32(w, (uint32_t)node->size) != 0) {
            return -1;
        }
        if (node->size > 0 && w_bytes(w, node->data, node->size) != 0) {
            return -1;
        }
        return 0;
    }

    uint32_t child_count = 0;
    for (struct vnode *c = node->children; c != NULL; c = c->sibling) {
        child_count++;
    }
    if (w_u32(w, child_count) != 0) {
        return -1;
    }
    for (struct vnode *c = node->children; c != NULL; c = c->sibling) {
        if (serialize_node(w, c) != 0) {
            return -1;
        }
    }
    return 0;
}

/* --- Deserialization: a read cursor over a fixed buffer. --- */

struct reader {
    const uint8_t *buf;
    uint64_t pos;
    uint64_t len;
};

static int r_bytes(struct reader *r, void *out, uint64_t len) {
    if (r->pos + len > r->len) {
        return -1;
    }
    if (out != NULL) {
        memcpy(out, r->buf + r->pos, len);
    }
    r->pos += len;
    return 0;
}

static int r_u8(struct reader *r, uint8_t *out) {
    return r_bytes(r, out, sizeof(*out));
}

static int r_u32(struct reader *r, uint32_t *out) {
    return r_bytes(r, out, sizeof(*out));
}

/* `parent` is where this record's node should be created; NULL means
 * "this record IS the root" (which vfs_init() already created -- skip
 * creating it, just consume its record and recurse into its children). */
static int deserialize_node(struct reader *r, struct vnode *parent) {
    uint8_t type;
    uint8_t name_len;
    char name[VFS_NAME_MAX];

    if (r_u8(r, &type) != 0 || r_u8(r, &name_len) != 0 || name_len >= sizeof(name)) {
        return -1;
    }
    if (r_bytes(r, name, name_len) != 0) {
        return -1;
    }
    name[name_len] = '\0';

    if (type == VNODE_FILE) {
        uint32_t size;
        if (r_u32(r, &size) != 0 || parent == NULL) {
            return -1; /* A file can't be the top-level record. */
        }
        if (vfs_create_file(parent, name) != 0) {
            return -1;
        }
        if (size == 0) {
            return 0;
        }
        uint8_t *content = kmalloc(size);
        if (content == NULL || r_bytes(r, content, size) != 0 ||
            vfs_write_bytes(parent, name, content, size) != 0) {
            kfree(content);
            return -1;
        }
        kfree(content);
        return 0;
    }

    struct vnode *dir;
    if (parent == NULL) {
        dir = vfs_root();
    } else {
        if (vfs_mkdir(parent, name) != 0) {
            return -1;
        }
        dir = vfs_resolve(parent, name);
        if (dir == NULL) {
            return -1;
        }
    }

    uint32_t child_count;
    if (r_u32(r, &child_count) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < child_count; i++) {
        if (deserialize_node(r, dir) != 0) {
            return -1;
        }
    }
    return 0;
}

int blkfs_load(void) {
    if (!virtio_blk_is_ready()) {
        return 0;
    }

    uint8_t *sb_buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (sb_buf == NULL) {
        kprintf("blkfs: out of memory\n");
        return -1;
    }
    if (virtio_blk_read(0, sb_buf, 1) != 0) {
        kprintf("blkfs: failed to read the superblock\n");
        kfree(sb_buf);
        return -1;
    }
    struct blkfs_superblock sb;
    memcpy(&sb, sb_buf, sizeof(sb));
    kfree(sb_buf);

    if (sb.magic != BLKFS_MAGIC) {
        kprintf("blkfs: no filesystem on disk yet -- will format on first save\n");
        return 0;
    }
    if (sb.version != BLKFS_VERSION) {
        kprintf("blkfs: on-disk version %u unsupported (expected %u) -- ignoring\n", sb.version,
                BLKFS_VERSION);
        return 0;
    }
    if (sb.total_size == 0) {
        kprintf("blkfs: loaded empty filesystem\n");
        return 0;
    }

    uint64_t data_sectors = (sb.total_size + VIRTIO_BLK_SECTOR_SIZE - 1) / VIRTIO_BLK_SECTOR_SIZE;
    uint8_t *buf = kmalloc(data_sectors * VIRTIO_BLK_SECTOR_SIZE);
    if (buf == NULL) {
        kprintf("blkfs: out of memory (%lu bytes)\n", data_sectors * VIRTIO_BLK_SECTOR_SIZE);
        return -1;
    }
    if (virtio_blk_read(1, buf, (uint32_t)data_sectors) != 0) {
        kprintf("blkfs: failed to read filesystem data\n");
        kfree(buf);
        return -1;
    }

    struct reader r = {.buf = buf, .pos = 0, .len = sb.total_size};
    int result = deserialize_node(&r, NULL);
    kfree(buf);

    if (result != 0) {
        kprintf("blkfs: on-disk data is corrupt -- ignoring, starting with an empty filesystem\n");
        return -1;
    }

    kprintf("blkfs: loaded %lu bytes from disk\n", sb.total_size);
    return 0;
}

int blkfs_save(void) {
    if (!virtio_blk_is_ready()) {
        return 0;
    }

    struct writer measure = {.buf = NULL, .pos = 0, .cap = 0};
    if (serialize_node(&measure, vfs_root()) != 0) {
        kprintf("blkfs: failed to measure the filesystem\n");
        return -1;
    }
    uint64_t total_size = measure.pos;
    uint64_t data_sectors = (total_size + VIRTIO_BLK_SECTOR_SIZE - 1) / VIRTIO_BLK_SECTOR_SIZE;

    uint64_t capacity = virtio_blk_capacity_sectors();
    if (1 + data_sectors > capacity) {
        kprintf("blkfs: filesystem too large for the disk (%lu sectors needed, %lu available)\n",
                1 + data_sectors, capacity);
        return -1;
    }

    uint64_t buf_size = data_sectors > 0 ? data_sectors * VIRTIO_BLK_SECTOR_SIZE : 0;
    uint8_t *buf = buf_size > 0 ? kmalloc(buf_size) : NULL;
    if (buf_size > 0 && buf == NULL) {
        kprintf("blkfs: out of memory (%lu bytes)\n", buf_size);
        return -1;
    }
    if (buf != NULL) {
        memset(buf, 0, buf_size);
    }

    struct writer w = {.buf = buf, .pos = 0, .cap = buf_size};
    if (serialize_node(&w, vfs_root()) != 0) {
        kprintf("blkfs: failed to serialize the filesystem\n");
        kfree(buf);
        return -1;
    }

    uint8_t *sb_buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (sb_buf == NULL) {
        kprintf("blkfs: out of memory\n");
        kfree(buf);
        return -1;
    }
    memset(sb_buf, 0, VIRTIO_BLK_SECTOR_SIZE);
    struct blkfs_superblock *sb = (struct blkfs_superblock *)sb_buf;
    sb->magic = BLKFS_MAGIC;
    sb->version = BLKFS_VERSION;
    sb->total_size = total_size;

    int failed = 0;
    if (virtio_blk_write(0, sb_buf, 1) != 0) {
        failed = 1;
    }
    if (!failed && data_sectors > 0 && virtio_blk_write(1, buf, (uint32_t)data_sectors) != 0) {
        failed = 1;
    }

    kfree(sb_buf);
    kfree(buf);

    if (failed) {
        kprintf("blkfs: write failed\n");
        return -1;
    }

    kprintf("blkfs: saved %lu bytes (%lu sectors)\n", total_size, data_sectors);
    return 0;
}
