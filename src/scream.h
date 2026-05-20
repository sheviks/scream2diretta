#ifndef SCREAM_H
#define SCREAM_H

#include <stdint.h>
#include <signal.h>

enum receiver_type {
  Unicast, Multicast, SharedMem, Pcap
};

enum output_type {
  Raw, Alsa, Pulseaudio, Jack, Sndio, Diretta
};

typedef struct receiver_format {
  unsigned char sample_rate;
  unsigned char sample_size;
  unsigned char channels;
  uint16_t channel_map;
} receiver_format_t;

typedef struct receiver_data {
  receiver_format_t format;
  unsigned int audio_size;
  unsigned char* audio;
} receiver_data_t;

extern int verbosity;

/* Global shutdown flag set from a signal handler (SIGINT/SIGTERM).
 * Receiver implementations must check this on every blocking-syscall
 * EINTR or short-poll wakeup so that Ctrl-C exits within ~1s. */
extern volatile sig_atomic_t g_shutdown_pending;

#endif
