#include "vfs.h"
#include "../drivers/serial.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

#define VFS_PATH_MAX_DEPTH 64

static struct vnode *root;

static struct vnode *alloc_vnode(const char *name, enum vnode_type type) {
    struct vnode *node = kmalloc(sizeof(struct vnode));
    if (node == NULL) {
        kprintf("vfs: out of memory\n");
        return NULL;
    }

    size_t len = strlen(name);
    if (len >= VFS_NAME_MAX) {
        len = VFS_NAME_MAX - 1;
    }
    memcpy(node->name, name, len);
    node->name[len] = '\0';

    node->type = type;
    node->parent = NULL;
    node->children = NULL;
    node->sibling = NULL;
    node->data = NULL;
    node->size = 0;
    node->capacity = 0;
    return node;
}

static void link_child(struct vnode *parent, struct vnode *child) {
    child->parent = parent;
    child->sibling = parent->children;
    parent->children = child;
}

static void unlink_child(struct vnode *parent, struct vnode *child) {
    struct vnode **link = &parent->children;
    while (*link != NULL) {
        if (*link == child) {
            *link = child->sibling;
            child->sibling = NULL;
            child->parent = NULL;
            return;
        }
        link = &(*link)->sibling;
    }
}

static struct vnode *find_child(struct vnode *dir, const char *name) {
    for (struct vnode *c = dir->children; c != NULL; c = c->sibling) {
        if (strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return NULL;
}

static int is_ancestor_or_self(struct vnode *maybe_ancestor, struct vnode *node) {
    for (struct vnode *n = node; n != NULL; n = n->parent) {
        if (n == maybe_ancestor) {
            return 1;
        }
    }
    return 0;
}

void vfs_init(void) {
    root = alloc_vnode("", VNODE_DIR); /* Root has no name of its own. */
    kprintf("vfs: initialized (in-memory only -- resets on reboot)\n");
}

struct vnode *vfs_root(void) {
    return root;
}

struct vnode *vfs_resolve(struct vnode *base, const char *path) {
    struct vnode *cur = (path[0] == '/') ? root : base;
    if (path[0] == '/') {
        path++;
    }

    while (*path != '\0') {
        char component[VFS_NAME_MAX];
        size_t i = 0;
        while (path[i] != '\0' && path[i] != '/' && i + 1 < sizeof(component)) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';
        path += i;
        while (*path == '/') {
            path++;
        }

        if (component[0] == '\0' || strcmp(component, ".") == 0) {
            continue;
        }
        if (strcmp(component, "..") == 0) {
            if (cur->parent != NULL) {
                cur = cur->parent;
            }
            continue;
        }

        if (cur->type != VNODE_DIR) {
            return NULL;
        }
        struct vnode *next = find_child(cur, component);
        if (next == NULL) {
            return NULL;
        }
        cur = next;
    }

    return cur;
}

/* Splits `path` into its resolved parent directory and trailing leaf
 * name (copied into `leaf_out`). Returns NULL if the parent path doesn't
 * resolve. Trailing slashes are ignored ("foo/" behaves like "foo"). */
static struct vnode *resolve_parent_and_leaf(struct vnode *base, const char *path, char *leaf_out,
                                             size_t leaf_out_size) {
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        len--;
    }
    if (len == 0) {
        return NULL; /* "/" or "" -- no leaf to split off. */
    }

    size_t last_slash = (size_t)-1;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
        }
    }

    const char *leaf = (last_slash == (size_t)-1) ? path : path + last_slash + 1;
    size_t leaf_len = len - ((last_slash == (size_t)-1) ? 0 : last_slash + 1);
    if (leaf_len >= leaf_out_size) {
        leaf_len = leaf_out_size - 1;
    }
    memcpy(leaf_out, leaf, leaf_len);
    leaf_out[leaf_len] = '\0';

    if (last_slash == (size_t)-1) {
        return base; /* No slash at all -- parent is `base`. */
    }
    if (last_slash == 0) {
        return root; /* Path was "/leaf". */
    }

    char parent_path[256];
    size_t parent_len = last_slash;
    if (parent_len >= sizeof(parent_path)) {
        parent_len = sizeof(parent_path) - 1;
    }
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';
    return vfs_resolve(base, parent_path);
}

static int create_leaf(struct vnode *base, const char *path, enum vnode_type type,
                       const char *verb) {
    char leaf[VFS_NAME_MAX];
    struct vnode *parent = resolve_parent_and_leaf(base, path, leaf, sizeof(leaf));
    if (parent == NULL) {
        kprintf("%s: %s: no such directory\n", verb, path);
        return -1;
    }
    if (parent->type != VNODE_DIR) {
        kprintf("%s: %s: not a directory\n", verb, path);
        return -1;
    }
    if (leaf[0] == '\0') {
        kprintf("%s: %s: invalid name\n", verb, path);
        return -1;
    }
    if (find_child(parent, leaf) != NULL) {
        kprintf("%s: %s: already exists\n", verb, path);
        return -1;
    }

    struct vnode *node = alloc_vnode(leaf, type);
    if (node == NULL) {
        return -1;
    }
    link_child(parent, node);
    return 0;
}

int vfs_mkdir(struct vnode *base, const char *path) {
    return create_leaf(base, path, VNODE_DIR, "create");
}

int vfs_create_file(struct vnode *base, const char *path) {
    return create_leaf(base, path, VNODE_FILE, "create");
}

static void free_tree(struct vnode *node) {
    if (node->type == VNODE_DIR) {
        struct vnode *c = node->children;
        while (c != NULL) {
            struct vnode *next = c->sibling;
            free_tree(c);
            c = next;
        }
    } else if (node->data != NULL) {
        kfree(node->data);
    }
    kfree(node);
}

int vfs_remove(struct vnode *base, const char *path, struct vnode *cwd) {
    struct vnode *node = vfs_resolve(base, path);
    if (node == NULL) {
        kprintf("delete: %s: no such file or directory\n", path);
        return -1;
    }
    if (node == root) {
        kprintf("delete: cannot remove the root directory\n");
        return -1;
    }
    if (is_ancestor_or_self(node, cwd)) {
        kprintf("delete: %s: is the current directory, or a parent of it\n", path);
        return -1;
    }

    unlink_child(node->parent, node);
    free_tree(node);
    return 0;
}

static struct vnode *copy_recursive(struct vnode *src, const char *new_name) {
    struct vnode *copy = alloc_vnode(new_name, src->type);
    if (copy == NULL) {
        return NULL;
    }

    if (src->type == VNODE_FILE) {
        if (src->size > 0) {
            copy->data = kmalloc(src->size);
            if (copy->data == NULL) {
                kfree(copy);
                kprintf("copy: out of memory\n");
                return NULL;
            }
            memcpy(copy->data, src->data, src->size);
            copy->size = src->size;
            copy->capacity = src->size;
        }
        return copy;
    }

    for (struct vnode *c = src->children; c != NULL; c = c->sibling) {
        struct vnode *child_copy = copy_recursive(c, c->name);
        if (child_copy == NULL) {
            free_tree(copy); /* Partial copy -- clean it up rather than leave debris. */
            return NULL;
        }
        link_child(copy, child_copy);
    }
    return copy;
}

/* Shared by vfs_copy()/vfs_move(): if `dest_path` names an existing
 * directory, the target is `src`'s own name inside it; otherwise
 * `dest_path` is split into a parent + new leaf name via
 * resolve_parent_and_leaf(). */
static int resolve_target(struct vnode *base, const char *dest_path, struct vnode *src,
                          struct vnode **out_parent, char *out_leaf, size_t out_leaf_size) {
    struct vnode *dest_existing = vfs_resolve(base, dest_path);
    if (dest_existing != NULL && dest_existing->type == VNODE_DIR) {
        *out_parent = dest_existing;
        size_t len = strlen(src->name);
        if (len >= out_leaf_size) {
            len = out_leaf_size - 1;
        }
        memcpy(out_leaf, src->name, len);
        out_leaf[len] = '\0';
        return 0;
    }

    struct vnode *parent = resolve_parent_and_leaf(base, dest_path, out_leaf, out_leaf_size);
    if (parent == NULL || parent->type != VNODE_DIR) {
        return -1;
    }
    *out_parent = parent;
    return 0;
}

int vfs_copy(struct vnode *base, const char *src_path, const char *dest_path) {
    struct vnode *src = vfs_resolve(base, src_path);
    if (src == NULL) {
        kprintf("copy: %s: no such file or directory\n", src_path);
        return -1;
    }

    struct vnode *parent;
    char leaf[VFS_NAME_MAX];
    if (resolve_target(base, dest_path, src, &parent, leaf, sizeof(leaf)) != 0) {
        kprintf("copy: %s: no such directory\n", dest_path);
        return -1;
    }
    if (leaf[0] == '\0') {
        kprintf("copy: %s: invalid name\n", dest_path);
        return -1;
    }
    if (find_child(parent, leaf) != NULL) {
        kprintf("copy: %s: already exists\n", dest_path);
        return -1;
    }
    if (src->type == VNODE_DIR && is_ancestor_or_self(src, parent)) {
        kprintf("copy: cannot copy a directory into itself or a descendant\n");
        return -1;
    }

    struct vnode *copy = copy_recursive(src, leaf);
    if (copy == NULL) {
        return -1;
    }
    link_child(parent, copy);
    return 0;
}

int vfs_move(struct vnode *base, const char *src_path, const char *dest_path) {
    struct vnode *src = vfs_resolve(base, src_path);
    if (src == NULL) {
        kprintf("move: %s: no such file or directory\n", src_path);
        return -1;
    }
    if (src == root) {
        kprintf("move: cannot move the root directory\n");
        return -1;
    }

    struct vnode *parent;
    char leaf[VFS_NAME_MAX];
    if (resolve_target(base, dest_path, src, &parent, leaf, sizeof(leaf)) != 0) {
        kprintf("move: %s: no such directory\n", dest_path);
        return -1;
    }
    if (leaf[0] == '\0') {
        kprintf("move: %s: invalid name\n", dest_path);
        return -1;
    }
    struct vnode *existing = find_child(parent, leaf);
    if (existing != NULL && existing != src) {
        kprintf("move: %s: already exists\n", dest_path);
        return -1;
    }
    if (src->type == VNODE_DIR && is_ancestor_or_self(src, parent)) {
        kprintf("move: cannot move a directory into itself or a descendant\n");
        return -1;
    }

    unlink_child(src->parent, src);
    size_t len = strlen(leaf);
    if (len >= VFS_NAME_MAX) {
        len = VFS_NAME_MAX - 1;
    }
    memcpy(src->name, leaf, len);
    src->name[len] = '\0';
    link_child(parent, src);
    return 0;
}

int vfs_write_file(struct vnode *base, const char *path, const char *text, int append) {
    struct vnode *node = vfs_resolve(base, path);
    if (node == NULL) {
        if (vfs_create_file(base, path) != 0) {
            return -1;
        }
        node = vfs_resolve(base, path);
        if (node == NULL) {
            return -1;
        }
    }
    if (node->type != VNODE_FILE) {
        kprintf("write: %s: not a file\n", path);
        return -1;
    }

    size_t text_len = strlen(text);
    size_t new_size = append ? node->size + text_len : text_len;

    uint8_t *buf = kmalloc(new_size > 0 ? new_size : 1);
    if (buf == NULL) {
        kprintf("write: out of memory\n");
        return -1;
    }

    if (append && node->size > 0) {
        memcpy(buf, node->data, node->size);
        memcpy(buf + node->size, text, text_len);
    } else {
        memcpy(buf, text, text_len);
    }

    if (node->data != NULL) {
        kfree(node->data);
    }
    node->data = buf;
    node->size = new_size;
    node->capacity = new_size;
    return 0;
}

int vfs_cat(struct vnode *base, const char *path) {
    struct vnode *node = vfs_resolve(base, path);
    if (node == NULL) {
        kprintf("cat: %s: no such file or directory\n", path);
        return -1;
    }
    if (node->type != VNODE_FILE) {
        kprintf("cat: %s: not a file\n", path);
        return -1;
    }

    for (size_t i = 0; i < node->size; i++) {
        kprintf("%c", node->data[i]);
    }
    kprintf("\n");
    return 0;
}

void vfs_list(struct vnode *base, const char *path) {
    struct vnode *dir = (path == NULL || path[0] == '\0') ? base : vfs_resolve(base, path);
    if (dir == NULL) {
        kprintf("ls: %s: no such file or directory\n", path);
        return;
    }
    if (dir->type != VNODE_DIR) {
        kprintf("ls: %s: not a directory\n", path);
        return;
    }

    int count = 0;
    for (struct vnode *c = dir->children; c != NULL; c = c->sibling) {
        kprintf("  %s%s\n", c->name, c->type == VNODE_DIR ? "/" : "");
        count++;
    }
    if (count == 0) {
        kprintf("  (empty)\n");
    }
}

void vfs_path(struct vnode *node, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    if (node == root) {
        out[0] = '/';
        if (out_size > 1) {
            out[1] = '\0';
        } else {
            out[0] = '\0';
        }
        return;
    }

    const struct vnode *chain[VFS_PATH_MAX_DEPTH];
    int depth = 0;
    for (const struct vnode *n = node; n != NULL && n != root && depth < VFS_PATH_MAX_DEPTH;
         n = n->parent) {
        chain[depth++] = n;
    }

    size_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (pos + 1 >= out_size) {
            break;
        }
        out[pos++] = '/';
        size_t nlen = strlen(chain[i]->name);
        if (pos + nlen >= out_size) {
            nlen = out_size - 1 - pos;
        }
        memcpy(out + pos, chain[i]->name, nlen);
        pos += nlen;
    }
    out[pos] = '\0';
}
