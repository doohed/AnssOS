# Syscalls

Entry is `int 0x80` -- vector `0x80` is the one deliberately `DPL=3`
gate in the IDT, and the only way into the kernel from ring 3. There is
no `SYSCALL`/`SYSRET` fast path yet. `exec/syscall.c` dispatches on the
number in `rax`; arguments follow the ordinary System V register order
(`rdi`, `rsi`, `rdx`).

**The numbers are Linux's real ones.** Reusing them costs nothing now
and avoids a renumbering exercise if a Linux-binary-compatibility layer
is ever attempted. The same reasoning applies to the flag and struct
values below -- `O_CREAT` is `0x40`, `TCGETS` is `0x5401`, `struct
termios` has Linux's layout, and so on.

## Table

| № | Name | Signature | Notes |
|---|---|---|---|
| 0 | `read` | `read(fd, buf, len)` | fd 0 is the console; `ICANON` selects line vs raw mode |
| 1 | `write` | `write(fd, buf, len)` | fd 1/2 go to the console, one flush per call |
| 2 | `open` | `open(path, flags)` | `O_RDONLY`/`O_WRONLY`, plus `O_CREAT` and `O_TRUNC` |
| 3 | `close` | `close(fd)` | |
| 8 | `lseek` | `lseek(fd, offset, whence)` | `SEEK_END` rejected on directory fds |
| 12 | `brk` | `brk(new_end)` | grows a page at a time, capped at 4 MiB, never shrinks |
| 16 | `ioctl` | `ioctl(fd, req, argp)` | `TCGETS`, `TCSETS`, `TIOCGWINSZ` only |
| 39 | `getpid` | `getpid()` | |
| 57 | `fork` | `fork()` | full page-by-page address-space copy, no copy-on-write |
| 59 | `execve` | `execve(path, argv)` | replaces the image in place; same pid, open files, cwd, termios |
| 60 | `exit` | `exit(status)` | |
| 61 | `wait4` | `waitpid(pid, status)` | simplified: no options, no `WNOHANG` |
| 79 | `getcwd` | `getcwd(buf, size)` | absolute path of the task's cwd |
| 80 | `chdir` | `chdir(path)` | also the only reliable "is this a directory?" test |
| 83 | `mkdir` | `mkdir(path)` | |
| 217 | `getdents` | `getdents(fd, dirent*)` | **one entry per call**, not Linux's batched-buffer ABI |

Everything resolves paths against the calling task's cwd, so relative
paths behave as they do in the shell.

## The initial stack

`elf_load()` builds a System V AMD64 process-initialization stack. At
entry `RSP` points at `argc`:

```
RSP ->  argc
        argv[0]
        ...
        argv[argc-1]
        NULL              <- argv terminator
        NULL              <- envp[0]; there is no environment yet
        0, 0              <- auxv AT_NULL
        ... argument strings ...
```

`crt0.S` reads `argc` into `%rdi` and `argv` into `%rsi`, aligns the
stack to 16 bytes, and calls `main`. Programs declaring `main(void)`
simply ignore the two registers, which is why adding argv did not
require touching every existing payload.

It is written through the HHDM rather than through the new address
space: the stack pages come from a single `pmm_alloc_pages()` run, so
they are physically contiguous and reachable without switching `CR3`.

Arguments are capped at 16 of 128 bytes each, bounds-checked against the
16 KiB user stack.

## Terminal handling

`struct usertask` carries a `termios`, defaulted to `ICANON|ECHO` and
explicitly preserved across `exec()` -- real Unix semantics, since
terminal settings belong to the terminal, not the program. `fork()`
inherits it for free via the existing shallow copy.

`read()` on fd 0 branches on `ICANON`. Raw mode does no echo and no
backspace handling, and returns as soon as one byte is available
(`VMIN=1`, `VTIME=0` -- the one concrete combination implemented).

`TIOCGWINSZ` answers from the framebuffer console's glyph grid, or
80x24 on a boot with no virtio-gpu. Note that it reports the
*framebuffer* geometry even when you are on the serial console, where
the real terminal may be a different size -- it is the only size the
kernel knows.

## Not implemented

No `mmap`, no signals, no `poll`/`select`, no `pipe`, no `dup`/`dup2`,
no pty, no `O_NONBLOCK`, no `stat`/`fstat`, no `unlink`/`rename`, no
`envp`, no TLS or `arch_prctl`. That list is why a real terminal
multiplexer, a shell running as a userland process, and a musl port are
all still out of reach -- see [roadmap.md](roadmap.md).
