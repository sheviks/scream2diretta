#include "network.h"
#include "stdio.h"

#include <errno.h>
#include <sys/select.h>

static rctx_network_t rctx_network;

int init_network(enum receiver_type receiver_mode, in_addr_t interface, int port,
                 char* multicast_group, int udp_rcvbuf_bytes,
                 const char* allowed_source_ip)
{
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

void rcv_network(receiver_data_t* receiver_data)
{
  ssize_t n = 0;
  receiver_data->audio_size = 0;
  receiver_data->audio = NULL;

  // Use select() with a 500ms timeout so the receiver loop can observe a
  // pending shutdown even if no audio packets are flowing. recvfrom() with
  // an installed SIGINT handler would otherwise simply return EINTR and we
  // used to loop on that without checking the shutdown flag, which made
  // Ctrl-C ineffective.
  while (n < HEADER_SIZE) {
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

    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    n = recvfrom(rctx_network.sockfd, rctx_network.buf, MAX_SO_PACKETSIZE, 0,
                 (struct sockaddr *)&sender, &sender_len);
    if (n < 0) {
      if (errno == EINTR) return;
      continue;
    }
    if (rctx_network.allowed_addr_set) {
      if (sender.sin_addr.s_addr != rctx_network.allowed_addr.sin_addr.s_addr) {
        // Silently drop packet from non-allowed source.
        continue;
      }
    }
  }
  receiver_data->format.sample_rate = rctx_network.buf[0];
  receiver_data->format.sample_size = rctx_network.buf[1];
  receiver_data->format.channels = rctx_network.buf[2];
  receiver_data->format.channel_map = (rctx_network.buf[4] << 8) | rctx_network.buf[3];
  receiver_data->audio_size = n - HEADER_SIZE;
  receiver_data->audio = &rctx_network.buf[5];
}

