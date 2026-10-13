//! In-memory filesystem: a tree of vnodes in the kernel heap, reset on
//! every boot unless fs/blkfs.rs loads a saved one. Every operation
//! reports its own error via the kernel log and returns false,
//! rather than a rich error-code API.
//!
//! The tree is linked by raw pointers -- parent, first child, next
//! sibling -- since a node's place in it changes (move) while processes
//! and open files point at it; a node lives until it's removed.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::ptr::null_mut;

use crate::drivers::serial::kout;
use crate::sync::Global;

/// Longest name plus a NUL: getdents() hands names to userland in a
/// NAME_MAX-byte field.
pub const NAME_MAX: usize = 64;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum VnodeType {
    Dir = 0,
    File = 1,
}

pub struct Vnode {
    name: String,
    pub kind: VnodeType,
    pub parent: *mut Vnode,
    /// First child (directories only).
    pub children: *mut Vnode,
    /// Next sibling within the parent's children list.
    pub sibling: *mut Vnode,
    /// File content (files only).
    pub data: Vec<u8>,
}

static ROOT: Global<*mut Vnode> = Global::new(null_mut());

pub fn root() -> *mut Vnode {
    unsafe { *ROOT.get() }
}

impl Vnode {
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Names are cut to NAME_MAX - 1 bytes, at a character boundary.
    fn set_name(&mut self, name: &str) {
        self.name = String::from(&name[..name.floor_char_boundary(NAME_MAX - 1)]);
    }

    pub fn is_dir(&self) -> bool {
        self.kind == VnodeType::Dir
    }

    /// The children, in list order (newest first).
    pub fn children(&self) -> impl Iterator<Item = &'static mut Vnode> {
        let mut c = self.children;
        core::iter::from_fn(move || {
            let node = unsafe { c.as_mut() }?;
            c = node.sibling;
            Some(node)
        })
    }

    fn find_child(&self, name: &str) -> Option<&'static mut Vnode> {
        self.children().find(|c| c.name() == name)
    }
}

fn alloc_vnode(name: &str, kind: VnodeType) -> *mut Vnode {
    let mut node = Box::new(Vnode {
        name: String::new(),
        kind,
        parent: null_mut(),
        children: null_mut(),
        sibling: null_mut(),
        data: Vec::new(),
    });
    node.set_name(name);
    Box::into_raw(node)
}

fn link_child(parent: *mut Vnode, child: *mut Vnode) {
    unsafe {
        (*child).parent = parent;
        (*child).sibling = (*parent).children;
        (*parent).children = child;
    }
}

fn unlink_child(parent: *mut Vnode, child: *mut Vnode) {
    unsafe {
        let mut link = &raw mut (*parent).children;
        while !(*link).is_null() {
            if *link == child {
                *link = (*child).sibling;
                (*child).sibling = null_mut();
                (*child).parent = null_mut();
                return;
            }
            link = &raw mut (**link).sibling;
        }
    }
}

fn is_ancestor_or_self(ancestor: *mut Vnode, node: *mut Vnode) -> bool {
    let mut n = node;
    while !n.is_null() {
        if n == ancestor {
            return true;
        }
        n = unsafe { (*n).parent };
    }
    false
}

pub fn init() {
    unsafe { *ROOT.get() = alloc_vnode("", VnodeType::Dir) }; // The root has no name of its own.
    kprintln!("vfs: initialized (in-memory only -- resets on reboot)");
}

/// Resolves an absolute ("/a/b") or relative (to `base`) path, handling
/// "." and "..". None (no message -- "not found" is a normal, expected
/// outcome callers handle themselves) if any component doesn't exist, or
/// a non-leaf component isn't a directory.
pub fn resolve(base: *mut Vnode, path: &str) -> Option<*mut Vnode> {
    let mut cur = if path.starts_with('/') { root() } else { base };
    for component in path.split('/') {
        // Overlong components are cut, like names are when created.
        let component = &component[..component.floor_char_boundary(NAME_MAX - 1)];
        match component {
            "" | "." => {}
            ".." => {
                let parent = unsafe { (*cur).parent };
                if !parent.is_null() {
                    cur = parent;
                }
            }
            _ => {
                let dir = unsafe { &*cur };
                if !dir.is_dir() {
                    return None;
                }
                cur = dir.find_child(component)?;
            }
        }
    }
    Some(cur)
}

/// Splits `path` into its resolved parent directory and trailing leaf
/// name. None if the parent path doesn't resolve. Trailing slashes are
/// ignored ("foo/" behaves like "foo").
fn resolve_parent_and_leaf(base: *mut Vnode, path: &str) -> Option<(*mut Vnode, String)> {
    let path = path.trim_end_matches('/');
    if path.is_empty() {
        return None; // "/" or "" -- no leaf to split off.
    }
    let (parent, leaf) = match path.rfind('/') {
        None => (Some(base), path),            // No slash at all -- the parent is `base`.
        Some(0) => (Some(root()), &path[1..]), // "/leaf".
        Some(i) => (resolve(base, &path[..i]), &path[i + 1..]),
    };
    Some((parent?, String::from(&leaf[..leaf.floor_char_boundary(NAME_MAX - 1)])))
}

fn create_leaf(base: *mut Vnode, path: &str, kind: VnodeType, verb: &str) -> bool {
    let Some((parent, leaf)) = resolve_parent_and_leaf(base, path) else {
        kprintln!("{verb}: {path}: no such directory");
        return false;
    };
    let p = unsafe { &*parent };
    if !p.is_dir() {
        kprintln!("{verb}: {path}: not a directory");
        return false;
    }
    if leaf.is_empty() {
        kprintln!("{verb}: {path}: invalid name");
        return false;
    }
    if p.find_child(&leaf).is_some() {
        kprintln!("{verb}: {path}: already exists");
        return false;
    }
    link_child(parent, alloc_vnode(&leaf, kind));
    true
}

/// Both resolve `path`'s parent (which must already exist -- no `-p` style
/// recursive creation) and create the leaf. They fail if it exists.
pub fn mkdir(base: *mut Vnode, path: &str) -> bool {
    create_leaf(base, path, VnodeType::Dir, "create")
}

pub fn create_file(base: *mut Vnode, path: &str) -> bool {
    create_leaf(base, path, VnodeType::File, "create")
}

fn free_tree(node: *mut Vnode) {
    let n = unsafe { &mut *node };
    if n.is_dir() {
        let mut c = n.children;
        while !c.is_null() {
            let next = unsafe { (*c).sibling };
            free_tree(c);
            c = next;
        }
    }
    drop(unsafe { Box::from_raw(node) });
}

/// Removes a file, or a directory and everything under it. Refuses (and
/// says why) for the root, or for `cwd` or an ancestor of it -- removing
/// those would leave the caller holding a dangling pointer.
pub fn remove(base: *mut Vnode, path: &str, cwd: *mut Vnode) -> bool {
    let Some(node) = resolve(base, path) else {
        kprintln!("delete: {path}: no such file or directory");
        return false;
    };
    if node == root() {
        kprintln!("delete: cannot remove the root directory");
        return false;
    }
    if is_ancestor_or_self(node, cwd) {
        kprintln!("delete: {path}: is the current directory, or a parent of it");
        return false;
    }
    unlink_child(unsafe { (*node).parent }, node);
    free_tree(node);
    true
}

fn copy_recursive(src: &Vnode, new_name: &str) -> Option<*mut Vnode> {
    let copy = alloc_vnode(new_name, src.kind);
    if !src.is_dir() {
        unsafe { (*copy).data = src.data.clone() };
        return Some(copy);
    }
    for c in src.children() {
        let Some(child) = copy_recursive(c, c.name()) else {
            free_tree(copy); // A partial copy -- clean it up rather than leave debris.
            return None;
        };
        link_child(copy, child);
    }
    Some(copy)
}

/// Shared by copy()/move_node(): if `dest_path` names an existing
/// directory, the target is `src`'s own name inside it; otherwise
/// `dest_path` is the new parent + leaf name.
fn resolve_target(base: *mut Vnode, dest_path: &str, src: &Vnode) -> Option<(*mut Vnode, String)> {
    if let Some(dest) = resolve(base, dest_path)
        && unsafe { (*dest).is_dir() }
    {
        return Some((dest, String::from(src.name())));
    }
    resolve_parent_and_leaf(base, dest_path).filter(|(parent, _)| unsafe { (**parent).is_dir() })
}

/// If `dest_path` is an existing directory, the source lands inside it
/// under its own name (the cp/mv-into-directory convention); otherwise
/// `dest_path` is the full new parent + name. Refuses if the final name
/// already exists (no silent overwrite or merge) or, for directories, if
/// the destination is the source itself or one of its descendants.
pub fn copy(base: *mut Vnode, src_path: &str, dest_path: &str) -> bool {
    let Some(src) = resolve(base, src_path) else {
        kprintln!("copy: {src_path}: no such file or directory");
        return false;
    };
    let s = unsafe { &*src };
    let Some((parent, leaf)) = resolve_target(base, dest_path, s) else {
        kprintln!("copy: {dest_path}: no such directory");
        return false;
    };
    if leaf.is_empty() {
        kprintln!("copy: {dest_path}: invalid name");
        return false;
    }
    if unsafe { (*parent).find_child(&leaf) }.is_some() {
        kprintln!("copy: {dest_path}: already exists");
        return false;
    }
    if s.is_dir() && is_ancestor_or_self(src, parent) {
        kprintln!("copy: cannot copy a directory into itself or a descendant");
        return false;
    }
    let Some(copy) = copy_recursive(s, &leaf) else { return false };
    link_child(parent, copy);
    true
}

pub fn move_node(base: *mut Vnode, src_path: &str, dest_path: &str) -> bool {
    let Some(src) = resolve(base, src_path) else {
        kprintln!("move: {src_path}: no such file or directory");
        return false;
    };
    if src == root() {
        kprintln!("move: cannot move the root directory");
        return false;
    }
    let s = unsafe { &mut *src };
    let Some((parent, leaf)) = resolve_target(base, dest_path, s) else {
        kprintln!("move: {dest_path}: no such directory");
        return false;
    };
    if leaf.is_empty() {
        kprintln!("move: {dest_path}: invalid name");
        return false;
    }
    if let Some(existing) = unsafe { (*parent).find_child(&leaf) }
        && !core::ptr::eq(existing, src)
    {
        kprintln!("move: {dest_path}: already exists");
        return false;
    }
    if s.is_dir() && is_ancestor_or_self(src, parent) {
        kprintln!("move: cannot move a directory into itself or a descendant");
        return false;
    }
    unlink_child(s.parent, src);
    s.set_name(&leaf);
    link_child(parent, src);
    true
}

/// Replaces a file's content with `data`, creating the file first if it
/// doesn't exist yet.
pub fn write_bytes(base: *mut Vnode, path: &str, data: &[u8]) -> bool {
    let node = match resolve(base, path) {
        Some(n) => n,
        None => {
            if !create_file(base, path) {
                return false;
            }
            let Some(n) = resolve(base, path) else { return false };
            n
        }
    };
    let n = unsafe { &mut *node };
    if n.is_dir() {
        kprintln!("write: {path}: not a file");
        return false;
    }
    n.data = data.to_vec();
    true
}

/// Overwrites (or appends to, if `append`) a file's content, creating it
/// first if it doesn't exist yet.
pub fn write_file(base: *mut Vnode, path: &str, text: &[u8], append: bool) -> bool {
    let existing = resolve(base, path).map(|n| unsafe { &*n }).filter(|n| append && !n.is_dir());
    match existing {
        Some(n) => write_bytes(base, path, &[n.data.as_slice(), text].concat()),
        None => write_bytes(base, path, text),
    }
}

/// Prints a file's content to the kernel log, then a newline.
pub fn cat(base: *mut Vnode, path: &str) -> bool {
    let Some(node) = resolve(base, path) else {
        kprintln!("cat: {path}: no such file or directory");
        return false;
    };
    let n = unsafe { &*node };
    if n.is_dir() {
        kprintln!("cat: {path}: not a file");
        return false;
    }
    // Byte by byte: the content needn't be UTF-8.
    n.data.iter().for_each(|&b| kout(b));
    kprintln!();
    true
}

/// Lists `path`'s children (or `base`'s, if `path` is empty), one per
/// line, directories suffixed with '/'.
pub fn list(base: *mut Vnode, path: &str) {
    let dir = if path.is_empty() { Some(base) } else { resolve(base, path) };
    let Some(dir) = dir.map(|d| unsafe { &*d }) else {
        kprintln!("ls: {path}: no such file or directory");
        return;
    };
    if !dir.is_dir() {
        kprintln!("ls: {path}: not a directory");
        return;
    }
    let mut count = 0;
    for c in dir.children() {
        kprintln!("  {}{}", c.name(), if c.is_dir() { "/" } else { "" });
        count += 1;
    }
    if count == 0 {
        kprintln!("  (empty)");
    }
}

/// The full path to `node`, rebuilt by walking parent links.
pub fn path(node: *mut Vnode) -> String {
    let mut parts = Vec::new();
    let mut n = node;
    while !n.is_null() && n != root() {
        parts.push(unsafe { (*n).name() });
        n = unsafe { (*n).parent };
    }
    if parts.is_empty() {
        return String::from("/");
    }
    parts.iter().rev().fold(String::new(), |mut s, p| {
        s.push('/');
        s.push_str(p);
        s
    })
}
