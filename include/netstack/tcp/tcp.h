#ifndef NETSTACK_TCP_H
#define NETSTACK_TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include <netstack/log.h>
#include <netstack/inet.h>
#include <netstack/inet/neigh.h>
#include <netstack/intf/intf.h>
#include <netstack/col/llist.h>
#include <netstack/col/seqbuf.h>
#include <netstack/time/timer.h>
#include <netstack/time/contimer.h>
#include <netstack/lock/retlock.h>

// Global TCP states list
extern llist_t tcp_sockets;

/*
    Source: https://tools.ietf.org/html/rfc793#page-15

     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |          Source Port          |       Destination Port        |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                        Sequence Number                        |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                    Acknowledgment Number                      |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |  Data |           |U|A|P|R|S|F|                               |
    | Offset| Reserved  |R|C|S|S|Y|I|            Window             |
    |       |           |G|K|H|T|N|N|                               |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |           Checksum            |         Urgent Pointer        |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                    Options                    |    Padding    |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                             data                              |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

struct tcp_hdr {
    // TODO: Take endianness into account in tcp_hdr
    uint16_t    sport,          /* Source port */
                dport;          /* Destination port */
    uint32_t    seqn,           /* Sequence number */
                ackn;           /* Acknowledgement number */
#if THE_HOST_IS_BIG_ENDIAN
    uint8_t     hlen:4,         /* Size of TCP header in 32-bit words */
                rsvd:4;         /* Empty reserved space for ctrl flags */
    union {
        struct tcp_flags {
                    /* Ignoring the experimental NS bit here: RFC 3540 */
        uint8_t     cwr:1,
                    ece:1,
                    urg:1,
                    ack:1,           /* Various control bits */
                    psh:1,
                    rst:1,
                    syn:1,
                    fin:1;
#else
    /* Ignoring the experimental NS bit here: RFC 3540 */
    uint8_t     rsvd:4,         /* Empty reserved space for ctrl flags */
                hlen:4;         /* Size of TCP header in 32-bit words */
    union {
        struct tcp_flags {
            uint8_t fin:1,
                    syn:1,
                    rst:1,
                    psh:1,
                    ack:1,           /* Various control bits */
                    urg:1,
                    ece:1,
                    cwr:1;
#endif
        } flags;
        uint8_t flagval;
    };
    uint16_t    wind,           /* Size of the receive window */
                csum,           /* Internet checksum */
                urg_ptr;        /* Urgent data pointer */

    /* Options go here */

}__attribute((packed));

struct tcb {
    uint32_t irs;       // initial receive sequence number
    uint32_t iss;       // initial send sequence number
    // Send Sequence Variables
    struct tcb_snd {
        uint32_t una;   // send unacknowledged
        uint32_t nxt;   // send next
        uint16_t wnd;   // send window (what it is: https://tools.ietf.org/html/rfc793#page-20)
        uint16_t up;    // send urgent pointer
        uint32_t wl1;   // segment sequence number used for last window update
        uint32_t wl2;   // segment acknowledgment number used for last window update
    } snd;
    // Receive Sequence Variables
    struct tcb_rcv {
        uint32_t nxt;   // receive next
        uint16_t wnd;   // receive window
        uint16_t up;    // receive urgent pointer
    } rcv;
};


// Use tcp_state enumeration defined in <netinet/tcp.h> for compatibility
#include <netinet/tcp.h>

#define tcp_state_t         unsigned char
#define TCP_SYN_RECEIVED    TCP_SYN_RECV
#define TCP_FIN_WAIT_1      TCP_FIN_WAIT1
#define TCP_FIN_WAIT_2      TCP_FIN_WAIT2
#define TCP_CLOSED          TCP_CLOSE


static const char *tcp_strstate(tcp_state_t state) {
    switch (state) {
        case TCP_LISTEN:        return "LISTEN";
        case TCP_SYN_SENT:      return "SYN-SENT";
        case TCP_SYN_RECEIVED:  return "SYN-RECEIVED";
        case TCP_ESTABLISHED:   return "ESTABLISHED";
        case TCP_FIN_WAIT_1:    return "FIN-WAIT-1";
        case TCP_FIN_WAIT_2:    return "FIN-WAIT-2";
        case TCP_CLOSE_WAIT:    return "CLOSE-WAIT";
        case TCP_CLOSING:       return "CLOSING";
        case TCP_CLOSED:        return "CLOSED";
        case TCP_LAST_ACK:      return "LAST-ACK";
        case TCP_TIME_WAIT:     return "TIME-WAIT";
        default:                return NULL;
    }
}

struct tcp_passive {
    size_t maxbacklog;          // Maximum amount of backlog clients acceptable
    llist_t backlog;            // List of clients waiting to be accept'ed.
};

struct tcp_sock {
    struct inet_sock inet;
    tcp_state_t state;
    struct tcb tcb;
    uint16_t mss;               // Defaults to TCP_DEF_MSS if not "negotiated"
                                // MSS for _outgoing_ send() calls only!

    struct tcp_passive *passive;// Non-NULL when the connection is PASSIVE (LISTEN)
    struct tcp_sock *parent;

    // Data buffers
    seqbuf_t sndbuf;           // Sent data, stored in case of retransmissions
    llist_t recvqueue;          // llist<struct frame> of recv'd tcp frames
    uint32_t recvptr;           // Pointer to next byte to be recv'd

    // Retransmission
    contimer_t rtimer;           // Retransmission timeout
    contimer_event_t rto_event;  // Timer event corresponding to rto
    llist_t unacked;             // Sequence numbers of unacknowledged segments

    struct timespec rto;         // Retransmit timeout value. Calculated from rtt
    struct timespec lasttime;    // Timestamp at which the last rto was started
    uint64_t rtt, srtt, rttvar;  // Round-trip time values for retransmission
    uint16_t backoff;

    // TCP timers
    timeout_t timewait;

    // Reference counting & shared-locking
    atomic_int refcount;
    int error;

    // Thread wait lock
    pthread_mutex_t lock;
    pthread_cond_t wait;
    pthread_cond_t waitack;
};


// TODO: Fix endianness in tcp.h
#if THE_HOST_IS_BIG_ENDIAN
#define TCP_FLAG_CWR    0x01
#define TCP_FLAG_ECE    0x02
#define TCP_FLAG_URG    0x04
#define TCP_FLAG_ACK    0x08
#define TCP_FLAG_PSH    0x10
#define TCP_FLAG_RST    0x20
#define TCP_FLAG_SYN    0x40
#define TCP_FLAG_FIN    0x80
#else
#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20
#define TCP_FLAG_ECE    0x40
#define TCP_FLAG_CWR    0x80
#endif


#define TCP_DEF_MSS     536     // MSS conservative default as per RFC879
                                // https://tools.ietf.org/html/rfc879
#define TCP_MSL         60      // Maximum Segment Lifetime (in seconds)

// Initial connect timeout. Is doubled every retry
#define TCP_SYN_RTO     mstons((uint64_t) 500U)
// Amount of retries before giving up
#define TCP_SYN_COUNT   6

// Minimum RTO in nanoseconds. RFC 6298 (2.4) says: 'RTO _SHOULD_ be rounded up to 1 second'
// https://tools.ietf.org/html/rfc6298#page-3
// Linux uses a minimum RTO of 200 ms
#define TCP_RTO_MIN     mstons((uint64_t) 100U)


/* Returns a string of characters/dots representing a set/unset TCP flag */
static inline char *fmt_tcp_flags(uint8_t flags, char *buffer) {
    if (buffer == NULL) {
        return NULL;
    }
    if (flags == 0) {
        buffer[0] = '\0';
        return buffer;
    }

    uint8_t count = 0;
    if (flags & TCP_FLAG_FIN) buffer[count++] = 'F';
    if (flags & TCP_FLAG_SYN) buffer[count++] = 'S';
    if (flags & TCP_FLAG_RST) buffer[count++] = 'R';
    if (flags & TCP_FLAG_PSH) buffer[count++] = 'P';
    if (flags & TCP_FLAG_ACK) buffer[count++] = 'A';
    if (flags & TCP_FLAG_URG) buffer[count++] = 'U';
    if (flags & TCP_FLAG_ECE) buffer[count++] = 'E';
    if (flags & TCP_FLAG_CWR) buffer[count++] = 'C';
    buffer[count] = '\0';

    return buffer;
}


/* Returns a struct tcp_hdr from the frame->head */
#define tcp_hdr(frame) ((struct tcp_hdr *) (frame)->head)

/* Returns the size in bytes of a header
 * hdr->hlen is 1 byte, soo 4x is 1 word size */
#define tcp_hdr_len(hdr) ((uint16_t) ((hdr)->hlen * 4))

#define tcp_mss_ipv4(intf) (uint16_t) ((intf)->mtu) - \
                                sizeof(struct tcp_hdr) - \
                                sizeof(struct ipv4_hdr)

bool tcp_log(struct pkt_log *log, struct frame *frame, uint16_t net_csum,
             addr_t addr1, addr_t addr2);

/* Logs all frames in the recvqueue of a socket with LVERB */
void tcp_log_recvqueue(struct tcp_sock *sock);

/* Receives a tcp frame given an ipv4 parent */
void tcp_ipv4_recv(struct frame *frame, struct ipv4_hdr *hdr);

/* Receives a tcp frame for processing in the network stack */
void tcp_recv(struct frame *frame, struct tcp_sock *sock, uint16_t net_csum);

/*!
 * Sets the state of the TCP connection
 * Checks and performs queued actions on the socket
 * @param sock  socket to change state of
 * @param state state to set
 */
#define tcp_setstate(sock, state) \
        LOG(LDBUG, "%s state reached", tcp_strstate(state)); \
        _tcp_setstate(sock, state)

void _tcp_setstate(struct tcp_sock *sock, tcp_state_t state);

/*!
 * Called on a newly established connection. It allocates required buffers
 * for data transmission
 * @param sock  socket to initialise
 * @param recvnext sequence number to expect the first byte at
 */
void tcp_established(struct tcp_sock *sock, uint32_t recvnext);

/*!
 * Finds a matching tcp_sock with address/port quad, including matching
 * against wildcard addresses and ports.
 * @see inet_sock_lookup
 * @return a tcp_sock instance, or NULL if no matches found
 */
static inline struct tcp_sock *tcp_sock_lookup(addr_t *remaddr, addr_t *locaddr,
                                               uint16_t remport, uint16_t locport) {
    return (struct tcp_sock *)
            inet_sock_lookup(&tcp_sockets, remaddr, locaddr, remport, locport);
}

/*!
 * Removes a socket from the global socket list
 * Should be called before tcp_free_sock() to avoid race conditions
 */
#define tcp_sock_untrack(sock) llist_remove(&tcp_sockets, (sock))

/*!
 * Initialises tcp_sock variables
 * @param sock sock to initialise
 * @return pointer to sock, initialised
 */
struct tcp_sock *tcp_sock_init(struct tcp_sock *sock);

/*!
 * Removes a TCP socket from the global socket list and deallocates it
 * The sock->lock should be held for writing before calling this
 * @param sock socket to free
 */
void tcp_sock_free(struct tcp_sock *sock);

/*!
 * Deallocates a socket. Does NOT remove it from the global socket list
 * @param sock socket to free
 */
void tcp_sock_destroy(struct tcp_sock *sock);

/*!
 * Cleans up incomplete operations such as still-open connections, waiting
 * send/recv calls and calls tcp_free_sock()
 * @param sock
 */
void tcp_sock_abort(struct tcp_sock *sock);

/*!
 * Holds a reference to the tcp socket. Every reference obtained should be
 * released. Unreleased references prevent the socket memory being released.
 * Note: There is no requirement to hold sock->lock as this operation is atomic
 */
int _tcp_sock_incref(struct tcp_sock *sock, const char *file, int line, const char *func);
#define tcp_sock_incref(sock) _tcp_sock_incref(sock, __FILE__, __LINE__, __func__)

/*!
 * Releases the socket reference. When the refcount hits 0, the socket is
 * free'd. This function does not release the mutex if it is held and assumes
 * that no other thread will try to access the socket memory after it returns;
 */
int _tcp_sock_decref(struct tcp_sock *sock, const char *file, int line, const char *func);
#define tcp_sock_decref(sock) _tcp_sock_decref(sock, __FILE__, __LINE__, __func__)

/*!
 * Releases the socket reference. When the refcount hits 0, the socket is
 * free'd. This function should be called with the sock->lock held. It is assumed
 * that no other thread will try to access the socket memory after it returns;
 * Note: The shared lock sock->lock is released by this function in either case
 */
int _tcp_sock_decref_unlock(struct tcp_sock *sock, const char *file, int line, const char *func);
#define tcp_sock_decref_unlock(sock) _tcp_sock_decref_unlock(sock, __FILE__, __LINE__, __func__)

#define tcp_sock_lock(sock) pthread_mutex_lock(&(sock)->lock)

#define tcp_sock_trylock(sock) pthread_mutex_trylock(&(sock)->lock)

#define tcp_sock_unlock(sock) pthread_mutex_unlock(&(sock)->lock)


#define tcp_set_error(sock, err) (sock)->error = (err);
#define tcp_wait_change(sock) pthread_cond_wait(&(sock)->wait, &(sock)->lock)
#define tcp_wake_waiters(sock) pthread_cond_broadcast(&(sock)->wait)
static inline int tcp_wake_error(struct tcp_sock *sock, int error) {
    tcp_set_error(sock, error);
    return pthread_cond_broadcast(&(sock)->wait);
}

/*
 * TCP Internet functions
 */

/*!
 * Returns a new random open outgoing TCP port
 * @return a random port number
 */
uint16_t tcp_randomport();

/*!
 * Returns a new random initial sequence/acknowledgement number
 * @return a random seq/ack number
 */
uint32_t tcp_seqnum();


/*
 * TCP Utility functions
 */

/*!
 * Compares two TCP headers by the sequence numbers, used for sorting segements
 * into sequence order
 */
int tcp_seg_cmp(const struct frame *a, const struct frame *b);

/*!
 * Gets the next sequence number after the longest contiguous sequence of bytes
 * stored in sock->recvqueue after the initial value given.
 * @param init initial sequence number to count from (next byte expected)
 * @return the next byte after the highest contiguous sequence number from
 *         init that is held in sock->recvqueue
 */
uint32_t tcp_recvqueue_contigseq(struct tcp_sock *sock, uint32_t init);


/*
 * TCP Sequence number arithmetic
 */
#define tcp_ack_acceptable(tcb, ackn) \
    tcp_seq_leq((tcb)->snd.una, (ackn)) && tcp_seq_leq((ackn), (tcb)->snd.nxt)

// The ACK for the FIN is the sequence number _after_ the last byte sent.
// This can only be reached when SND.NXT is incremented when the FIN is sent
#define tcp_fin_was_acked(sock) \
    (sock)->tcb.snd.una == (uint32_t) ((sock)->sndbuf.start + (sock)->sndbuf.count + 1)

#define tcp_seq_lt(a, b) (((int32_t) (a)) - ((int32_t) (b)) < 0)
#define tcp_seq_gt(a, b) (((int32_t) (a)) - ((int32_t) (b)) > 0)
#define tcp_seq_leq(a, b) (((int32_t) (a)) - ((int32_t) (b)) <= 0)
#define tcp_seq_geq(a, b) (((int32_t) (a)) - ((int32_t) (b)) >= 0)

#define tcp_seq_inrange(seq, start, end) \
    (((seq) - (start)) < ((end) - (start)))
#define tcp_seq_inwnd(seq, start, size) \
    tcp_seq_inrange(seq, start, (start) + (size))


/*
 * TCP Input
 * See: tcpin.c
 */
void expand_escapes(char* dest, const char* src, size_t len);

void tcp_recv_closed(struct frame *frame, struct tcp_hdr *seg);

void tcp_recv_listen(struct frame *frame, struct tcp_sock *sock,
                     struct tcp_hdr *seg);

int tcp_seg_arr(struct frame *frame, struct tcp_sock *sock);

/*!
 * Updates the TCP send window from an incoming segment
 */
void tcp_update_wnd(struct tcb *tcb, struct tcp_hdr *seg);


/*
 * TCP Output
 * See: tcpout.c
 */

/*!
 * Computes the TCP header checksum for a complete TCP frame and sends it
 * according to the route information in rt.
 * @param sock internet socket that the frame refers to (from sock->inet)
 * @param frame frame to send
 * @return 0 on success, negative error otherwise
 */
int tcp_send(struct inet_sock *sock, struct frame *frame, struct neigh_route *rt);

/*!
 * Constructs and sends a TCP packet with an empty payload
 * @param sock TCP socket to send a packet for
 * @param seqn sequence number to put in the header
 * @param ackn acknowledgement number to put in the header
 * @param flags TCP state flags to set in the header
 * @return 0 on success, negative error otherwise
 */
int tcp_send_empty(struct tcp_sock *sock, uint32_t seqn, uint32_t ackn,
                   uint8_t flags);

/*!
 * Constructs and sends a TCP packet with the largest payload available to send,
 * or as much as can fit in a single packet, from the socket data send queue.
 * @param sock TCP socket to send a packet for
 * @param seqn sequence number to send data from
 * @param count hint for amount of bytes to send. 0 indicates automatic (as
 *              much as will fit in one packet)
 * @param flags optionally extra flags for the segment. ACK is implicitly added
 * @return >= 0: number of bytes sent, negative error otherwise
 */
int tcp_send_data(struct tcp_sock *sock, uint32_t seqn, size_t count, uint8_t flags);

/*!
 * Adds an outgoing segment to the unacked queue in case it is required for
 * later retransmission. This can be used for both data and control packets
 */
void tcp_queue_unacked(struct tcp_sock *sock, uint32_t seqn, uint16_t len,
                       uint8_t flags);

/*!
 * Given an uninitialised frame, a TCP segment is formed, allocating space for
 * data, header options and the header.
 * @param seg   segment to initialise
 * @param sock  socket to initialise segment for
 * @param seqn sequence number to put in the header
 * @param ackn acknowledgement number to put in the header
 * @param flags TCP state flags to set in the header
 * @param datalen largest payload size to allocate space for
 * @return >= 0: number of bytes allocated for segment payload, negative error otherwise
 */
int tcp_init_header(struct frame *seg, struct tcp_sock *sock, uint32_t seqn,
                     uint32_t ackn, uint8_t flags, size_t datalen);

/*!
 * Constructs a block of TCP options for a given socket
 * @param sock socket to construct options for
 * @param flags TCP state flags of segment header
 * @param opt buffer for option data. Should be large enough to accommodate all
 * header options (at most 40 bytes)
 * @return un-padded length of option block written to opt
 */
size_t tcp_options(struct tcp_sock *sock, uint8_t flags, uint8_t *opt);

/*!
 * Send a TCP ACK segment in the form
 *    <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
 */
#define tcp_send_ack(sock) \
    tcp_send_empty((sock), (sock)->tcb.snd.nxt, (sock)->tcb.rcv.nxt, \
        TCP_FLAG_ACK)

/*!
 * Send a TCP SYN segment in the form
 *    <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
 */
int tcp_send_syn(struct tcp_sock *sock);

/*!
 * Send a TCP SYN/ACK segment in the form
 *    <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
 */
#define tcp_send_synack(sock) \
    tcp_send_empty((sock), (sock)->tcb.iss, (sock)->tcb.rcv.nxt, \
        TCP_FLAG_SYN | TCP_FLAG_ACK)

/*!
 * Send a TCP FIN/ACK segment in the form
 *    <SEQ=SND.NXT><ACK=RCV.NXT><CTL=FIN,ACK>
 */
#define tcp_send_finack(sock) \
    tcp_send_empty((sock), (sock)->tcb.snd.nxt++, (sock)->tcb.rcv.nxt, \
        TCP_FLAG_FIN | TCP_FLAG_ACK)

/*!
 * Sends a TCP RST segment given a socket, in the form
 *    <SEQ={seqn}><CTL=RST>
 */
#define tcp_send_rst(sock, seq) \
    tcp_send_empty((sock), (seq), 0, TCP_FLAG_RST)

/*!
 * Sends a TCP RST/ACK segment given a socket, in the form
 *    <SEQ={seqn}><ACK={ackn}><CTL=RST,ACK>
 */
#define tcp_send_rstack(sock, seq, ack) \
    tcp_send_empty((sock), (seq), (ack), TCP_FLAG_RST | TCP_FLAG_ACK)


/*
 * TCP User (calls)
 * See: tcpuser.c
 */
int tcp_user_open(struct tcp_sock *sock);
int tcp_user_listen(struct tcp_sock *sock, size_t backlog);
int tcp_user_accept(struct tcp_sock *sock, struct tcp_sock **client);
int tcp_user_send(struct tcp_sock *sock, const void *data, size_t len, int flags);
int tcp_user_recv(struct tcp_sock *sock, void* data, size_t len, int flags);
int tcp_user_close(struct tcp_sock *sock);
int tcp_user_abort();
int tcp_user_status();


/*
 * TCP timers
 */
void tcp_timewait_expire(struct tcp_sock *sock);

#define tcp_timewait_start(sock) \
    timeout_set(&(sock)->timewait, (void(*)(void *)) tcp_timewait_expire, \
        (void *) (sock), TCP_MSL * 2, 0)

#define tcp_timewait_restart(sock) timeout_restart(&(sock)->timewait, -1, -1)

#define tcp_timewait_cancel(sock) timeout_clear(&(sock)->timewait)

#endif //NETSTACK_TCP_H
