#include "raw.h"

#include "receiver_tap.h"

static struct raw_output_data {
  receiver_format_t receiver_format;
  int rate;
  int bytes_per_sample;
} ro_data;

int raw_output_init()
{
  // init receiver format to track changes
  ro_data.receiver_format.sample_rate = 0;
  ro_data.receiver_format.sample_size = 0;
  ro_data.receiver_format.channels = 2;
  ro_data.receiver_format.channel_map = 0x0003;
  ro_data.receiver_format.wire_layout = 0;
  return 0;
}

int raw_output_send(receiver_data_t *data)
{
  receiver_format_t *rf = &data->format;

  if (memcmp(&ro_data.receiver_format, rf, sizeof(receiver_format_t))) {
    // audio format changed, reconfigure
    memcpy(&ro_data.receiver_format, rf, sizeof(receiver_format_t));

    /* sample_rate now holds decoded value (halved for DSD) */
    ro_data.rate = rf->sample_rate;
    switch (rf->sample_size) {
      case 16:
      case 24:
      case 32:
        break;
      case 1:
        /* DSD: double the rate (driver encodes half-rate); raw just passes bytes through */
        ro_data.rate *= 2;
        break;
      default:
        if (verbosity > 0) {
          fprintf(stderr, "Unsupported sample size %hhu, not playing until next format switch.\n", rf->sample_size);
        }
        ro_data.rate = 0;
    }

    ro_data.bytes_per_sample = scream_bytes_per_sample(rf);

    if (verbosity > 0) {
      const char *fmt_name = (rf->sample_size == 1) ? "DSD" :
                             (rf->sample_size == 24 && rf->wire_layout) ? "S24_LE" :
                             (rf->sample_size == 24) ? "S24_3LE" : "PCM";
      fprintf(stderr, "Switched to sample rate %u, sample size %hhu (%s), %u channels.\n",
              ro_data.rate, rf->sample_size, fmt_name, rf->channels);
    }

    if (rf->channels > 2) {
      int k = -1;
      for (int i=0; i<rf->channels; i++) {
        for (int j = k+1; j<=10; j++) {// check the channel map bit by bit from lsb to msb, starting from were we left on the previous step
          if ((rf->channel_map >> j) & 0x01) {// if the bit in j position is set then we have the key for this channel
            k = j;
            break;
          }
        }
        if (verbosity > 0) {
          const char *channel_name;
          switch (k) {
            case  0: channel_name = "Front Left"; break;
            case  1: channel_name = "Front Right"; break;
            case  2: channel_name = "Front Center"; break;
            case  3: channel_name = "LFE / Subwoofer"; break;
            case  4: channel_name = "Rear Left"; break;
            case  5: channel_name = "Rear Right"; break;
            case  6: channel_name = "Front-Left Center"; break;
            case  7: channel_name = "Front-Right Center"; break;
            case  8: channel_name = "Rear Center"; break;
            case  9: channel_name = "Side Left"; break;
            case 10: channel_name = "Side Right"; break;
            default:
              channel_name = "Unknown. Setted to Center.";
          }
          fprintf(stderr, "Channel %i is %s\n", i, channel_name);
        }
      }
    }
  }

  if (!ro_data.rate) return 0;

  // Raw-stdout WAV tap + startup analyser + receiver-tap comparator
  // feed. Tapped at the EXACT byte stream this function fwrites to stdout,
  // so the captured WAV is byte-perfect with `-o stdout > /tmp/foo.raw`
  // followed by an external raw->WAV conversion -- but without the manual
  // conversion step, removing one source of human error from the
  // diagnostic. No-op unless --dump-raw-stdout-wav is set or
  // --compare-receiver-tap-ms is active with -o stdout.
  //
  // Fast-path: skip the call entirely when no diagnostic facility is
  // armed. The static inline gate folds to a single int load + branch.
  if (receiver_tap_any_armed()) {
    receiver_tap_raw_feed(data->audio, data->audio_size,
                          (uint32_t)ro_data.rate,
                          (uint32_t)rf->sample_size,
                          (uint32_t)rf->channels);
  }

  fwrite(data->audio, 1, data->audio_size, stdout);

  return 0;
}
