#include "virtio_snd.h"
#include "virtio.h"
#include "../pci.h"
#include "../serial.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

/* Protocol structs/constants below match Linux's real
 * include/uapi/linux/virtio_snd.h (also the virtio-v1.2 spec, "Sound
 * Device") -- read in full rather than guessed, same "reuse the real
 * values" reasoning docs/syscalls.md already gives for Linux syscall
 * numbers. */

#define VIRTIO_SND_VQ_CONTROL 0
#define VIRTIO_SND_VQ_TX 2

#define VIRTIO_SND_D_OUTPUT 0

#define VIRTIO_SND_R_PCM_INFO 0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE 0x0102
#define VIRTIO_SND_R_PCM_RELEASE 0x0103
#define VIRTIO_SND_R_PCM_START 0x0104
#define VIRTIO_SND_R_PCM_STOP 0x0105

#define VIRTIO_SND_S_OK 0x8000

#define VIRTIO_SND_PCM_FMT_S16 5

#define VIRTIO_SND_PCM_RATE_44100 6
#define VIRTIO_SND_PCM_RATE_48000 7

#define PERIOD_BYTES 4096
#define BUFFER_BYTES (8 * PERIOD_BYTES)

struct __attribute__((packed)) virtio_snd_config {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
    uint32_t controls;
};

struct __attribute__((packed)) virtio_snd_hdr {
    uint32_t code;
};

struct __attribute__((packed)) virtio_snd_query_info {
    struct virtio_snd_hdr hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
};

struct __attribute__((packed)) virtio_snd_info {
    uint32_t hda_fn_nid;
};

struct __attribute__((packed)) virtio_snd_pcm_info {
    struct virtio_snd_info hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
};

/* Per the spec (5.14.6.2, "Driver Requirements: Item Information
 * Request"): the response buffer for an *_INFO query must be
 * sizeof(virtio_snd_hdr) + count * size, not just the raw item struct --
 * a leading status header precedes the payload, same shape every other
 * control response uses. Getting this wrong doesn't crash anything (QEMU
 * just rejects the request as BAD_MSG and never writes the payload), but
 * it means reading .info afterward reads uninitialized memory instead of
 * a real answer -- worth getting right rather than relying on the
 * response.code check below to save it. */
struct __attribute__((packed)) virtio_snd_pcm_info_resp {
    struct virtio_snd_hdr hdr;
    struct virtio_snd_pcm_info info;
};

struct __attribute__((packed)) virtio_snd_pcm_hdr {
    struct virtio_snd_hdr hdr;
    uint32_t stream_id;
};

struct __attribute__((packed)) virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
};

struct __attribute__((packed)) virtio_snd_pcm_xfer {
    uint32_t stream_id;
};

struct __attribute__((packed)) virtio_snd_pcm_status {
    uint32_t status;
    uint32_t latency_bytes;
};

static struct virtio_device vdev;
static struct virtio_queue controlq;
static struct virtio_queue txq;
static int initialized;
static int stream_open;

/* Scratch buffers, kmalloc'd once and reused -- same reasoning
 * virtio_blk.c's req_hdr/req_status comment already gives: requests here
 * are synchronous/one at a time, so there's never more than one in
 * flight. */
static struct virtio_snd_hdr *ctrl_resp;
static struct virtio_snd_pcm_xfer *tx_hdr;
static struct virtio_snd_pcm_status *tx_status;

/* virtio DMA needs an HHDM-mapped address (see virtio.h's struct
 * virtio_buffer doc comment: physical = virtual - hhdm_offset) --
 * virtio_snd_write()'s caller is audio_write(), a syscall whose `buf`
 * argument is a *userland* pointer, in a completely different
 * per-process address space with no such relationship to hhdm_offset.
 * DMAing from it directly reads whatever physical page that arithmetic
 * happens to land on -- wrong, but not a crash, which is what made this
 * one easy to miss until the captured audio came back silent. Every tx
 * chunk gets memcpy'd through this kmalloc'd (HHDM-backed) bounce buffer
 * first. */
static uint8_t *tx_bounce;

static int pcm_ctrl(uint32_t code, uint32_t stream_id, uint32_t buffer_bytes,
                     uint32_t period_bytes, uint8_t channels, uint8_t format, uint8_t rate) {
    struct virtio_snd_pcm_set_params req = {0};
    req.hdr.hdr.code = code;
    req.hdr.stream_id = stream_id;
    req.buffer_bytes = buffer_bytes;
    req.period_bytes = period_bytes;
    req.channels = channels;
    req.format = format;
    req.rate = rate;

    /* SET_PARAMS uses every field above; PREPARE/START/STOP/RELEASE only
     * look at the leading virtio_snd_pcm_hdr, so sending the same
     * (mostly zeroed) struct for all of them is harmless -- the device
     * only reads as many bytes as the request actually needs. */
    ctrl_resp->code = 0xFFFFFFFF;
    struct virtio_buffer bufs[2] = {
        {.addr = &req, .len = sizeof(req), .device_writable = 0},
        {.addr = ctrl_resp, .len = sizeof(*ctrl_resp), .device_writable = 1},
    };
    virtio_queue_submit_chain(&controlq, bufs, 2);
    virtio_queue_wait(&controlq);

    return (ctrl_resp->code == VIRTIO_SND_S_OK) ? 0 : -1;
}

static int pcm_info_is_output(uint32_t stream_id) {
    struct virtio_snd_query_info req = {0};
    req.hdr.code = VIRTIO_SND_R_PCM_INFO;
    req.start_id = stream_id;
    req.count = 1;
    req.size = sizeof(struct virtio_snd_pcm_info);

    struct virtio_snd_pcm_info_resp resp;
    resp.hdr.code = 0xFFFFFFFF;
    struct virtio_buffer bufs[2] = {
        {.addr = &req, .len = sizeof(req), .device_writable = 0},
        {.addr = &resp, .len = sizeof(resp), .device_writable = 1},
    };
    virtio_queue_submit_chain(&controlq, bufs, 2);
    virtio_queue_wait(&controlq);

    return resp.hdr.code == VIRTIO_SND_S_OK && resp.info.direction == VIRTIO_SND_D_OUTPUT;
}

int virtio_snd_init(void) {
    const struct pci_device *pci =
        pci_find_device(VIRTIO_SND_PCI_VENDOR_ID, VIRTIO_SND_PCI_DEVICE_ID);
    if (pci == NULL) {
        kprintf("virtio-snd: no sound device found -- boot QEMU with -device virtio-sound-pci\n");
        return -1;
    }

    if (virtio_pci_init(pci, &vdev) != 0 || virtio_negotiate_features(&vdev, 0) != 0 ||
        virtio_queue_init(&vdev, VIRTIO_SND_VQ_CONTROL, &controlq) != 0 ||
        virtio_queue_init(&vdev, VIRTIO_SND_VQ_TX, &txq) != 0) {
        return -1;
    }
    virtio_driver_ok(&vdev);

    ctrl_resp = kmalloc(sizeof(*ctrl_resp));
    tx_hdr = kmalloc(sizeof(*tx_hdr));
    tx_status = kmalloc(sizeof(*tx_status));
    tx_bounce = kmalloc(PERIOD_BYTES);
    if (ctrl_resp == NULL || tx_hdr == NULL || tx_status == NULL || tx_bounce == NULL) {
        kprintf("virtio-snd: out of memory\n");
        return -1;
    }

    volatile struct virtio_snd_config *cfg = (volatile struct virtio_snd_config *)vdev.device_cfg;
    uint32_t streams = cfg->streams;
    kprintf("virtio-snd: %u jack(s), %u stream(s), %u chmap(s)\n", cfg->jacks, streams,
            cfg->chmaps);

    if (streams == 0 || !pcm_info_is_output(0)) {
        kprintf("virtio-snd: stream 0 is not an output stream -- no playback available\n");
        return -1;
    }

    initialized = 1;
    kprintf("virtio-snd: ready\n");
    return 0;
}

int virtio_snd_is_ready(void) {
    return initialized;
}

int virtio_snd_open(uint32_t rate_hz, uint8_t channels) {
    if (!initialized) {
        return -1;
    }
    if (channels != 1 && channels != 2) {
        return -1;
    }
    uint8_t rate;
    if (rate_hz == 44100) {
        rate = VIRTIO_SND_PCM_RATE_44100;
    } else if (rate_hz == 48000) {
        rate = VIRTIO_SND_PCM_RATE_48000;
    } else {
        return -1;
    }

    if (pcm_ctrl(VIRTIO_SND_R_PCM_SET_PARAMS, 0, BUFFER_BYTES, PERIOD_BYTES, channels,
                 VIRTIO_SND_PCM_FMT_S16, rate) != 0) {
        kprintf("virtio-snd: PCM_SET_PARAMS failed\n");
        return -1;
    }
    if (pcm_ctrl(VIRTIO_SND_R_PCM_PREPARE, 0, 0, 0, 0, 0, 0) != 0) {
        kprintf("virtio-snd: PCM_PREPARE failed\n");
        return -1;
    }
    if (pcm_ctrl(VIRTIO_SND_R_PCM_START, 0, 0, 0, 0, 0, 0) != 0) {
        kprintf("virtio-snd: PCM_START failed\n");
        return -1;
    }

    stream_open = 1;
    return 0;
}

int virtio_snd_write(const void *pcm_s16le, uint32_t bytes) {
    if (!initialized || !stream_open) {
        return -1;
    }

    const uint8_t *p = (const uint8_t *)pcm_s16le;
    uint32_t sent = 0;
    while (sent < bytes) {
        uint32_t chunk = bytes - sent;
        if (chunk > PERIOD_BYTES) {
            chunk = PERIOD_BYTES;
        }

        tx_hdr->stream_id = 0;
        tx_status->status = 0xFFFFFFFF;
        memcpy(tx_bounce, p + sent, chunk);

        struct virtio_buffer bufs[3] = {
            {.addr = tx_hdr, .len = sizeof(*tx_hdr), .device_writable = 0},
            {.addr = tx_bounce, .len = chunk, .device_writable = 0},
            {.addr = tx_status, .len = sizeof(*tx_status), .device_writable = 1},
        };
        virtio_queue_submit_chain(&txq, bufs, 3);
        virtio_queue_wait(&txq);

        if (tx_status->status != VIRTIO_SND_S_OK) {
            return sent > 0 ? (int)sent : -1;
        }
        sent += chunk;
    }
    return (int)sent;
}

int virtio_snd_close(void) {
    if (!initialized || !stream_open) {
        return -1;
    }

    int ok = pcm_ctrl(VIRTIO_SND_R_PCM_STOP, 0, 0, 0, 0, 0, 0) == 0;
    ok = (pcm_ctrl(VIRTIO_SND_R_PCM_RELEASE, 0, 0, 0, 0, 0, 0) == 0) && ok;

    stream_open = 0;
    return ok ? 0 : -1;
}
