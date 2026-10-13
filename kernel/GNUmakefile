# Nuke built-in rules.
.SUFFIXES:

override OUTPUT := kernel

# Overridable from the environment, like scripts/build-userland.sh: the
# native ld on an x86_64 Linux host, ld.lld where docker/Dockerfile says so.
LD ?= ld

# The kernel is one Rust crate (Cargo.toml, src/), built as a static
# library for x86_64-unknown-none and linked here with linker.ld --
# Limine's higher-half layout. -u kmain is what pulls the crate out of the
# archive: a linker only takes an archive member that defines a symbol
# something already needs.
override LDFLAGS := \
    -m elf_x86_64 \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T linker.ld \
    -u kmain

RUST_LIB := target/x86_64-unknown-none/release/libkernel.a

.PHONY: all
all: bin/$(OUTPUT)

bin/$(OUTPUT): GNUmakefile linker.ld $(RUST_LIB)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(RUST_LIB) -o $@

# Cargo tracks its own dependencies -- the sources, and the userland
# binaries src/exec/blobs.rs embeds -- so it's simply asked every time;
# it finishes at once when nothing changed.
$(RUST_LIB): FORCE
	cargo build --release --quiet

.PHONY: FORCE
FORCE:

.PHONY: clean
clean:
	rm -rf bin target
