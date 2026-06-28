#include "network.h"
#include "stdio.h"

#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

static rctx_network_t rctx_network;
static int legacy_mode = 0;

int init_network(enum receiver_type receiver_mode, in_addr_t interface, int port,
                 char* multicast_group, int udp_rcvbuf_bytes,
                 const char* allowed_source_ip,
                 int udp_busy_poll_us, int enable_nic_timestamp,
                 int legacy)
{
  legacy_mode = legacy;
  rctx_network.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (rctx_network.sockfd < 0) {
    perror("Failed to craete socket");
    return 1;
  }

  /*  enlarge SO_RCVBUF so that bursts and short scheduler stalls do
   * not drop UDP packets at the kernel ingress. Userspace then drains
   * into the unified PCM ring. The kernel may cap the effective size
   * at net.core.rmem_max; we log what we actually got at -v. */
  if (udp_rcvbuf_bytes > 0) {
    int requested = udp_rcvbuf_bytes;
    if (setsockopt(rctx_network.sockfd, SOL_SOCKET, SO_RCVBUF,
                   &requested, sizeof(requested)) != 0) {
      perror("setsockopt SO_RCVBUF");
      /* not fatal: continue with kernel default */
    }
    if (verbosity) {
      int got = 0;
      socklen_t glen = sizeof(got);
      if (getsockopt(rctx_network.sockfd, SOL_SOCKET, SO_RCVBUF,
                     &got, &glen) == 0) {
        fprintf(stderr, "[scream2diretta] udp_rcvbuf requested=%d effective=%d "
                "(kernel may double the requested value)\n", requested, got);
      }
    }
  }

  /* SO_BUSY_POLL: ask the kernel to busy-poll the NIC RX queue from the
   * receiving syscall before sleeping. Eliminates scheduler wakeup
   * latency at the cost of one core spinning. Only safe on isolated
   * cores or when the user has dedicated CPU budget. Optional and
   * off by default; the value is the maximum poll window in
   * microseconds (typical: 50..100). */
  if (udp_busy_poll_us > 0) {
#ifdef SO_BUSY_POLL
    int v = udp_busy_poll_us;
    if (setsockopt(rctx_network.sockfd, SOL_SOCKET, SO_BUSY_POLL,
                   &v, sizeof(v)) != 0) {
      /* EPERM is the common case: requires CAP_NET_ADMIN.  We do not
       * fail init -- the receiver works without busy-poll, just with
       * normal scheduler wakeup behaviour. */
      fprintf(stderr, "[scream2diretta] WARNING: setsockopt(SO_BUSY_POLL=%d) "
              "failed: %s. Continuing without busy-poll. Hint: needs "
              "CAP_NET_ADMIN; run as root or grant the capability.\n",
              v, strerror(errno));
    } else if (verbosity) {
      int got = 0;
      socklen_t glen = sizeof(got);
      if (getsockopt(rctx_network.sockfd, SOL_SOCKET, SO_BUSY_POLL,
                     &got, &glen) == 0) {
        fprintf(stderr, "[scream2diretta] SO_BUSY_POLL requested=%d effective=%d us\n",
                v, got);
      }
    }
#ifdef SO_PREFER_BUSY_POLL
    /* Linux 5.11+: prefer busy-poll over softirq processing on this
     * socket so the polling thread fully drives the rx path. Best-
     * effort; failure (older kernels) is silent at default verbosity. */
    int prefer = 1;
    if (setsockopt(rctx_network.sockfd, SOL_SOCKET, SO_PREFER_BUSY_POLL,
                   &prefer, sizeof(prefer)) != 0) {
      if (verbosity) {
        fprintf(stderr, "[scream2diretta] SO_PREFER_BUSY_POLL not available: %s\n",
                strerror(errno));
      }
    } else if (verbosity) {
      fprintf(stderr, "[scream2diretta] SO_PREFER_BUSY_POLL enabled\n");
    }
#endif
#else
    fprintf(stderr, "[scream2diretta] WARNING: SO_BUSY_POLL not supported "
            "by this kernel header; --udp-busy-poll-us=%d ignored.\n",
            udp_busy_poll_us);
#endif
  }

  /* SO_TIMESTAMPNS: ask the kernel to attach a CLOCK_REALTIME nanosecond
   * timestamp (taken at sk_buff arrival, i.e. NIC -> kernel handoff) to
   * each datagram via SCM_TIMESTAMPNS in the cmsg ancillary buffer.
   * Lets stats distinguish upstream sender gaps from local userspace
   * wakeup jitter. Costs a few ns per packet for the cmsg copy. */
  rctx_network.nic_timestamp_enabled = 0;
  if (enable_nic_timestamp) {
#ifdef SO_TIMESTAMPNS
    int on = 1;
    if (setsockopt(rctx_network.sockfd, SOL_SOCKET, SO_TIMESTAMPNS,
                   &on, sizeof(on)) != 0) {
      fprintf(stderr, "[scream2diretta] WARNING: setsockopt(SO_TIMESTAMPNS) "
              "failed: %s. Continuing without NIC timestamps.\n",
              strerror(errno));
    } else {
      rctx_network.nic_timestamp_enabled = 1;
      if (verbosity) {
        fprintf(stderr, "[scream2diretta] SO_TIMESTAMPNS enabled "
                "(nic_gap_ms available in stats)\n");
      }
    }
#else
    fprintf(stderr, "[scream2diretta] WARNING: SO_TIMESTAMPNS not supported "
            "by this kernel header; --enable-nic-timestamp ignored.\n");
#endif
  }

  rctx_network.allowed_addr_set = 0;
  memset((void *)&(rctx_network.allowed_addr), 0, sizeof(rctx_network.allowed_addr));
  if (allowed_source_ip && allowed_source_ip[0]) {
      if (inet_pton(AF_INET, allowed_source_ip, &rctx_network.allowed_addr.sin_addr) == 1) {
          rctx_network.allowed_addr_set = 1;
          if (verbosity) {
              fprintf(stderr, "[scream2diretta] UDP source filter: only accepting packets from %s\n",
                      allowed_source_ip);
          }
      } else {
          fprintf(stderr, "[scream2diretta] WARNING: invalid --allowed-source-ip '%s', ignored\n",
                  allowed_source_ip);
      }
  }

  memset((void *)&(rctx_network.servaddr), 0, sizeof(rctx_network.servaddr));
  rctx_network.servaddr.sin_family = AF_INET;
  rctx_network.servaddr.sin_addr.s_addr = (receiver_mode == Unicast) ? interface : htonl(INADDR_ANY);
  rctx_network.servaddr.sin_port = htons(port);

  if (bind(rctx_network.sockfd, (struct sockaddr *)&rctx_network.servaddr, sizeof(rctx_network.servaddr)) != 0) {
    perror("Failed to bind to interface");
    return 1;
  }

  if (receiver_mode == Multicast) {
    rctx_network.imreq.imr_multiaddr.s_addr = inet_addr(multicast_group ? multicast_group : DEFAULT_MULTICAST_GROUP);
    rctx_network.imreq.imr_interface.s_addr = interface;

    if (setsockopt(rctx_network.sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                  (const void *)&rctx_network.imreq, sizeof(struct ip_mreq)) != 0) {
      perror("Failed to join multicast group");
      return 1;
    };
  }

  return 0;
}

/* Original Scream / ap2renderer sends DSD with left/right bytes interleaved
 * per 8-byte frame. Standard ALSA DSD_U32_BE expects each channel's 4 bytes
 * contiguous. This reverses the original driver-side convert_data() transform.
 */
static void dsd_legacy_deinterleave(char *src, size_t bytes)
{
  size_t frames = bytes / 8;
  while (frames--) {
    char tmp[8];
    /* incoming: [L0][R0][L1][R1][L2][R2][L3][R3] */
    tmp[0] = src[0];
    tmp[1] = src[2];
    tmp[2] = src[4];
    tmp[3] = src[6];
    tmp[4] = src[1];
    tmp[5] = src[3];
    tmp[6] = src[5];
    tmp[7] = src[7];
    memcpy(src, tmp, 8);
    src += 8;
  }
}

void rcv_network(receiver_data_t* receiver_data)
{
  ssize_t n = 0;
  receiver_data->audio_size = 0;
  receiver_data->audio = NULL;
  receiver_data->nic_timestamp_ns = 0;
  const size_t hdr_size = legacy_mode ? LEGACY_HEADER_SIZE : HEADER_SIZE;

  // Use select() with a 500ms timeout so the receiver loop can observe a
  // pending shutdown even if no audio packets are flowing. recvfrom() with
  // an installed SIGINT handler would otherwise simply return EINTR and we
  // used to loop on that without checking the shutdown flag, which made
  // Ctrl-C ineffective.
  while (n < (ssize_t)hdr_size) {
    if (g_shutdown_pending) return;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(rctx_network.sockfd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    int sr = select(rctx_network.sockfd + 1, &rfds, NULL, NULL, &tv);
    if (sr < 0) {
      if (errno == EINTR) {
        // signal — let main loop re-check g_shutdown_pending
        return;
      }
      // unexpected error: brief retry
      continue;
    }
    if (sr == 0) {
      // timeout: bubble up so the main loop can observe shutdown promptly
      return;
    }

    /* recvmsg path: lets us read SCM_TIMESTAMPNS out of the cmsg buffer
     * when --enable-nic-timestamp is in effect. The shape is identical
     * to the previous recvfrom() in everything else; the cmsg parse is
     * a few ns and is skipped entirely when nic_timestamp_enabled is
     * zero. */
    struct sockaddr_in sender;
    struct iovec iov;
    iov.iov_base = rctx_network.buf;
    iov.iov_len  = MAX_SO_PACKETSIZE;

    /* Cmsg buffer sized for SCM_TIMESTAMPNS (struct timespec).
     * CMSG_SPACE handles alignment + header overhead. */
    union {
      char buf[CMSG_SPACE(sizeof(struct timespec))];
      struct cmsghdr align;
    } cmsg_buf;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name       = &sender;
    msg.msg_namelen    = sizeof(sender);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);
    msg.msg_flags      = 0;

    n = recvmsg(rctx_network.sockfd, &msg, 0);
    if (n < 0) {
      if (errno == EINTR) return;
      continue;
    }
    if (rctx_network.allowed_addr_set) {
      if (sender.sin_addr.s_addr != rctx_network.allowed_addr.sin_addr.s_addr) {
        // Silently drop packet from non-allowed source.
        n = 0;
        continue;
      }
    }

    /* Extract NIC timestamp if requested and present. SCM_TIMESTAMPNS
     * carries struct timespec captured by the kernel at sk_buff arrival;
     * we serialise it as a single uint64_t nanoseconds since the
     * CLOCK_REALTIME epoch, which is what diretta.cpp consumes. A zero
     * value means "unavailable" -- propagate that semantics. */
    if (rctx_network.nic_timestamp_enabled) {
#ifdef SCM_TIMESTAMPNS
      for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != NULL;
           c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
          struct timespec ts;
          memcpy(&ts, CMSG_DATA(c), sizeof(ts));
          uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                        (uint64_t)ts.tv_nsec;
          /* Avoid 0 collision with "unavailable" sentinel; if the
           * kernel ever yielded a literal 0 timespec we round it up
           * to 1 ns. In practice CLOCK_REALTIME is far past the
           * epoch, so this branch is dead code on a sane system. */
          if (ns == 0) ns = 1;
          receiver_data->nic_timestamp_ns = ns;
          break;
        }
      }
#endif
    }
  }
  if (legacy_mode) {
    /* Original 5-byte Scream header:
     * byte[0]: rate code
     * byte[1]: sample size
     * byte[2]: channels
     * byte[3]: channel map low byte
     * byte[4]: channel map high byte
     */
    receiver_data->format.sample_rate = scream_decode_rate_legacy(rctx_network.buf[0]);
    receiver_data->format.sample_size = rctx_network.buf[1];
    receiver_data->format.channels = rctx_network.buf[2];
    receiver_data->format.channel_map = (rctx_network.buf[4] << 8) | rctx_network.buf[3];
    receiver_data->format.wire_layout = 0;
    receiver_data->audio_size = (n > LEGACY_HEADER_SIZE) ? (n - LEGACY_HEADER_SIZE) : 0;
    receiver_data->audio = &rctx_network.buf[LEGACY_HEADER_SIZE];

    /* Original Scream/ap2renderer DSD is byte-interleaved per frame.
     * Convert to standard ALSA DSD_U32_BE frame order on the fly.
     */
    if (receiver_data->format.sample_size == 1 && receiver_data->audio_size >= 8) {
      dsd_legacy_deinterleave((char*)receiver_data->audio, receiver_data->audio_size);
    }
    return;
  }

  /* Extended 6-byte ScreamALSA header:
   * byte[0]: rate low 8 bits
   * byte[1]: sample size
   * byte[2]: channels
   * byte[3]: channel map low byte
   * byte[4]: rate high 4 bits + 44.1k flag + end-of-track flag
   * byte[5]: wire_layout (only meaningful for 24-bit PCM)
   */
  receiver_data->format.sample_rate = scream_decode_rate(rctx_network.buf[0], rctx_network.buf[4]);
  receiver_data->format.sample_size = rctx_network.buf[1];
  receiver_data->format.channels = rctx_network.buf[2];
  receiver_data->format.channel_map = rctx_network.buf[3];
  receiver_data->format.wire_layout = rctx_network.buf[5];
  receiver_data->audio_size = (n > HEADER_SIZE) ? (n - HEADER_SIZE) : 0;
  receiver_data->audio = &rctx_network.buf[HEADER_SIZE];
}

