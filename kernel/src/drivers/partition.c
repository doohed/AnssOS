#include "partition.h"
#include "serial.h"
#include "../lib/crc32.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

struct __attribute__((packed)) gpt_header {
    char signature[8]; /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba, alternate_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
};

struct __attribute__((packed)) gpt_entry {
    uint8_t type[16];
    uint8_t unique[16];
    uint64_t first_lba, last_lba, attributes;
    uint16_t name[36]; /* UTF-16LE */
};

struct known_type {
    const char *guid; /* As printed: "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" */
    const char *name;
};

static const struct known_type GPT_TYPES[] = {
    {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system"},
    {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved"},
    {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data"},
    {"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery"},
    {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem"},
    {"4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)"},
    {"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap"},
    {"E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM"},
    {"A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID"},
    {"21686148-6449-6E6F-744E-656564454649", "BIOS boot"},
    {"48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+"},
};

/* GUIDs are stored with their first three fields little-endian. */
static void guid_string(const uint8_t *g, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[o++] = '-';
        }
        out[o++] = hex[g[order[i]] >> 4];
        out[o++] = hex[g[order[i]] & 0xF];
    }
    out[o] = '\0';
}

static void print_size(uint64_t bytes) {
    if (bytes >= (10ull << 30)) {
        kprintf("%lu GiB", bytes >> 30);
    } else if (bytes >= (10ull << 20)) {
        kprintf("%lu MiB", bytes >> 20);
    } else {
        kprintf("%lu KiB", bytes >> 10);
    }
}

/* What's in a partition, judged by its first 68 KiB. */
static const char *detect_fs(struct block_device *dev, uint64_t lba, uint64_t sectors) {
    uint32_t need = 68 * 1024;
    uint32_t count = (need + dev->sector_size - 1) / dev->sector_size;
    if (count > sectors) {
        count = (uint32_t)sectors;
    }
    uint8_t *b = kmalloc((uint64_t)count * dev->sector_size);
    if (b == NULL || block_read(dev, lba, count, b) != 0) {
        kfree(b);
        return "unreadable";
    }
    uint64_t have = (uint64_t)count * dev->sector_size;
    const char *fs = "";
    if (memcmp(b + 3, "NTFS    ", 8) == 0) {
        fs = "NTFS";
    } else if (memcmp(b + 3, "-FVE-FS-", 8) == 0) {
        fs = "BitLocker";
    } else if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
        fs = "exFAT";
    } else if (memcmp(b + 82, "FAT32   ", 8) == 0) {
        fs = "FAT32";
    } else if (memcmp(b + 54, "FAT1", 4) == 0) {
        fs = "FAT12/16";
    } else if (memcmp(b, "LUKS\xba\xbe", 6) == 0) {
        fs = "LUKS";
    } else if (have > 1082 && b[1080] == 0x53 && b[1081] == 0xEF) {
        fs = "ext2/3/4";
    } else if (have > 65608 && memcmp(b + 65600, "_BHRfS_M", 8) == 0) {
        fs = "btrfs";
    } else if (have > 32774 && memcmp(b + 32769, "CD001", 5) == 0) {
        fs = "ISO 9660";
    } else if (memcmp(b, "ANFS", 4) == 0) {
        fs = "AnssOS blkfs";
    } else if (memcmp(b, "XFSB", 4) == 0) {
        fs = "XFS";
    }
    kfree(b);
    return fs;
}

static void print_name(const uint16_t *name) {
    char s[37];
    int n = 0;
    for (int i = 0; i < 36 && name[i]; i++) {
        s[n++] = name[i] < 0x80 ? (char)name[i] : '?';
    }
    s[n] = '\0';
    if (n > 0) {
        kprintf(" \"%s\"", s);
    }
}

static int report_gpt(struct block_device *dev, const uint8_t *lba1) {
    const struct gpt_header *h = (const struct gpt_header *)lba1;
    if (memcmp(h->signature, "EFI PART", 8) != 0) {
        return -1;
    }
    struct gpt_header copy;
    memcpy(&copy, h, sizeof(copy));
    uint32_t header_size = h->header_size < sizeof(copy) ? h->header_size : sizeof(copy);
    copy.header_crc = 0;
    int header_ok = crc32(&copy, header_size) == h->header_crc;

    uint64_t array_bytes = (uint64_t)h->entry_count * h->entry_size;
    if (h->entry_size < sizeof(struct gpt_entry) || array_bytes > 1024 * 1024) {
        kprintf("  GPT with an implausible partition array -- not reading it\n");
        return 0;
    }
    uint64_t array_sectors = (array_bytes + dev->sector_size - 1) / dev->sector_size;
    uint8_t *array = kmalloc(array_sectors * dev->sector_size);
    if (array == NULL || block_read(dev, h->entries_lba, array_sectors, array) != 0) {
        kprintf("  GPT, but its partition array is unreadable\n");
        kfree(array);
        return 0;
    }
    int array_ok = crc32(array, array_bytes) == h->entries_crc;

    int used = 0;
    for (uint32_t i = 0; i < h->entry_count; i++) {
        const struct gpt_entry *e = (const struct gpt_entry *)(array + (uint64_t)i * h->entry_size);
        static const uint8_t zero[16];
        used += memcmp(e->type, zero, 16) != 0;
    }
    kprintf("  GPT: %d partitions, header CRC %s, partition array CRC %s\n", used,
            header_ok ? "ok" : "BAD", array_ok ? "ok" : "BAD");

    for (uint32_t i = 0; i < h->entry_count; i++) {
        const struct gpt_entry *e = (const struct gpt_entry *)(array + (uint64_t)i * h->entry_size);
        static const uint8_t zero[16];
        if (memcmp(e->type, zero, 16) == 0) {
            continue;
        }
        char guid[37];
        guid_string(e->type, guid);
        const char *type = guid;
        for (size_t t = 0; t < sizeof(GPT_TYPES) / sizeof(GPT_TYPES[0]); t++) {
            if (strcmp(guid, GPT_TYPES[t].guid) == 0) {
                type = GPT_TYPES[t].name;
            }
        }
        uint64_t sectors = e->last_lba >= e->first_lba ? e->last_lba - e->first_lba + 1 : 0;
        kprintf("  %sp%u: %s, ", dev->name, i + 1, type);
        print_size(sectors * dev->sector_size);
        const char *fs = sectors ? detect_fs(dev, e->first_lba, sectors) : "";
        if (fs[0]) {
            kprintf(", %s", fs);
        }
        uint16_t name[36];
        memcpy(name, e->name, sizeof(name));
        print_name(name);
        kprintf("\n");
    }
    kfree(array);
    return 0;
}

static void report_mbr(struct block_device *dev, const uint8_t *lba0) {
    if (lba0[510] != 0x55 || lba0[511] != 0xAA) {
        const char *fs = detect_fs(dev, 0, dev->sector_count);
        kprintf("  no partition table%s%s\n", fs[0] ? "; whole disk: " : "", fs);
        return;
    }
    kprintf("  MBR partition table\n");
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = lba0 + 446 + i * 16;
        uint8_t type = p[4];
        uint32_t start = (uint32_t)(p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24);
        uint32_t count = (uint32_t)(p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24);
        if (type == 0 || count == 0) {
            continue;
        }
        kprintf("  %sp%d: type 0x%x, ", dev->name, i + 1, type);
        print_size((uint64_t)count * dev->sector_size);
        const char *fs = start < dev->sector_count ? detect_fs(dev, start, count) : "";
        if (fs[0]) {
            kprintf(", %s", fs);
        }
        kprintf("\n");
    }
}

void partition_report(struct block_device *dev) {
    kprintf("%s: %s, ", dev->name, dev->model[0] ? dev->model : "(no model)");
    print_size(dev->sector_count * dev->sector_size);
    kprintf(", %u-byte sectors\n", dev->sector_size);

    uint8_t *b = kmalloc(2ull * dev->sector_size);
    if (b == NULL || block_read(dev, 0, 2, b) != 0) {
        kprintf("  can't read the first sectors\n");
        kfree(b);
        return;
    }
    if (report_gpt(dev, b + dev->sector_size) != 0) {
        report_mbr(dev, b);
    }
    kfree(b);
    /* A hybrid ISO (like the AnssOS live image) is also ISO 9660 from
     * sector 0, whatever partition table it carries for firmware. */
    if (strcmp(detect_fs(dev, 0, dev->sector_count), "ISO 9660") == 0) {
        kprintf("  whole disk: ISO 9660 (a hybrid ISO image, like the AnssOS live USB)\n");
    }
}
