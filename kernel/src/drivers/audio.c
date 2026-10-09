#include "audio.h"
#include "hda.h"
#include "virtio/virtio_snd.h"

#include <stddef.h>

static enum { AUDIO_NONE, AUDIO_VIRTIO, AUDIO_HDA } device;

int audio_init(void) {
    if (virtio_snd_init() == 0) {
        device = AUDIO_VIRTIO;
    } else if (hda_init() == 0) {
        device = AUDIO_HDA;
    } else {
        return -1;
    }
    return 0;
}

const char *audio_name(void) {
    switch (device) {
        case AUDIO_VIRTIO:
            return "virtio-sound";
        case AUDIO_HDA:
            return "HD Audio";
        default:
            return NULL;
    }
}

int audio_open(uint32_t rate_hz, uint8_t channels) {
    switch (device) {
        case AUDIO_VIRTIO:
            return virtio_snd_open(rate_hz, channels);
        case AUDIO_HDA:
            return hda_open(rate_hz, channels);
        default:
            return -1;
    }
}

int audio_write(const void *pcm_s16le, uint32_t bytes) {
    switch (device) {
        case AUDIO_VIRTIO:
            return virtio_snd_write(pcm_s16le, bytes);
        case AUDIO_HDA:
            return hda_write(pcm_s16le, bytes);
        default:
            return -1;
    }
}

int audio_close(void) {
    switch (device) {
        case AUDIO_VIRTIO:
            return virtio_snd_close();
        case AUDIO_HDA:
            return hda_close();
        default:
            return -1;
    }
}
