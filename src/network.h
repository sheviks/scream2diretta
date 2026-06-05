#ifndef NETWORK_H
#define NETWORK_H

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "scream.h"

#define DEFAULT_MULTICAST_GROUP "239.255.77.77"
#define DEFAULT_PORT 4010

#define HEADER_SIZE 5
#define MAX_SO_PACKETSIZE 1152+HEADER_SIZE

typedef struct rctx_network {
  int sockfd;
  struct sockaddr_in servaddr;
  struct ip_mreq imreq;
  unsigned char buf[MAX_SO_PACKETSIZE];
  struct sockaddr_in allowed_addr;   // allowed source IP (zero if unset)
  int allowed_addr_set;              // true when allowed_addr is valid
  int nic_timestamp_enabled;         // SO_TIMESTAMPNS active on sockfd
} rctx_network_t;

/* udp_rcvbuf_bytes selects the kernel SO_RCVBUF size applied to the
 * receive socket (0 = leave kernel default). The Diretta path benefits from
 * a deeper socket buffer so transient scheduler stalls do not drop UDP
 * packets before the userspace ring consumes them. Default in main() is
 * 4 MiB when output_mode is Diretta.
 *
 * udp_busy_poll_us > 0 enables SO_BUSY_POLL (and SO_PREFER_BUSY_POLL when
 * the kernel supports it). The receiver thread polls the NIC RX queue for
 * up to <us> before sleeping, eliminating scheduler wakeup jitter at the
 * cost of one core's worth of CPU.  Safe to enable on isolated cores.
 *
 * enable_nic_timestamp != 0 turns on SO_TIMESTAMPNS so each packet
 * arrives with the kernel's NIC-receive timestamp; it is exposed via
 * receiver_data_t::nic_timestamp_ns for downstream stats. */
int init_network(enum receiver_type receiver_mode, in_addr_t interface, int port,
                 char* multicast_group, int udp_rcvbuf_bytes,
                 const char* allowed_source_ip,
                 int udp_busy_poll_us, int enable_nic_timestamp);
void rcv_network(receiver_data_t* receiver_data);

#endif
