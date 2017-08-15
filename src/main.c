#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <time.h>

#include <sys/capability.h>
#include <linux/if_ether.h>

#include <libnet/frame.h>
#include <libnet/eth/ether.h>
#include <libnet/ip/ipv4.h>
#include <libnet/ip/ipproto.h>
#include <libnet/tcp/tcp.h>

int main(int argc, char **argv) {

    // Check for effective CAP_NET_RAW capability
    cap_flag_value_t hasRaw = CAP_CLEAR;
    cap_t capabilities = cap_get_proc();
    if (cap_get_flag(capabilities, CAP_NET_RAW, CAP_EFFECTIVE, &hasRaw)) {
        perror("Error checking capabilities");
    }
    cap_free(capabilities);
    if (hasRaw != CAP_SET) {
        fprintf(stderr, "You don't have the CAP_NET_RAW capability.\n"
                "Use 'setcap cap_net_raw+ep %s' or run as root\n", argv[0]);
        exit(1);
    }

    // Open a raw socket (raw layer 2/3 frames)
    // Use SOCK_DGRAM to remove ethernet header
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        perror("Error: could not create socket");
        return -1;
    }

    // Count is raw eth packet size (inc eth + ip headers)
    ssize_t lookahead = 0,
            count = 0;

    // Read data into the buffer
    while ((lookahead = recv(sock, NULL, 0, (MSG_PEEK | MSG_TRUNC))) != -1) {

        // Allocate a buffer of the correct size
        struct frame *eth_frame = init_frame(NULL, (size_t) lookahead);
        count = recv(sock, eth_frame->buffer, (size_t) lookahead, 0);

        if (count == -1) {
            perror("recv error");
        }
        // Warn if the sizes don't match (should probably never happen)
        //assert(count != lookahead);
        if (count != lookahead) {
            fprintf(stderr, "Warning: MSG_PEEK != recv(): %zi != %zi\n",
                    lookahead, count);
        }

        // Format and print time the same as tcpdump for comparison
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        char buf[15];
        strftime(buf, sizeof(buf), "%T", gmtime(&ts.tv_sec));
        snprintf(buf + 8, 8, ".%ld", ts.tv_nsec);
        // Print time received and payload size
        printf("%s Received a frame of %5lu bytes\t\n", buf, count);

        struct eth_hdr *eth_hdr = recv_ether(eth_frame);

        /*
         * ETHERNET
         */
        char ssaddr[18], sdaddr[18];
        fmt_mac(eth_hdr->saddr, ssaddr);
        fmt_mac(eth_hdr->daddr, sdaddr);
        printf("==> Ethernet Frame\n");
        printf("\tSource: %s\n\tDest:   %s\n\tType:   0x%04X > %s\n",
               ssaddr, sdaddr, eth_hdr->ethertype,
               fmt_ethertype(eth_hdr->ethertype));

        /*
         * IPv4
         */
        if (eth_hdr->ethertype == ETH_P_IP) {
            struct frame *ipv4_frame = frame_child_copy(eth_frame);
            struct ipv4_hdr *ipv4_hdr = recv_ipv4(ipv4_frame);

            char ip4_ssaddr[16], ip4_sdaddr[16];
            fmt_ipv4(ipv4_hdr->saddr, ip4_ssaddr);
            fmt_ipv4(ipv4_hdr->daddr, ip4_sdaddr);

            printf("\t==> IP Packet\n");
            printf("\t\tVersion:\t%u\n", ipv4_hdr->version);
            printf("\t\tIHL:\t\t%u words\n", ipv4_hdr->ihl);
            printf("\t\tLength:\t\t%u bytes\n", ipv4_hdr->len);
            printf("\t\tTTL:\t\t%u\n", ipv4_hdr->ttl);
            printf("\t\tProtocol:\t0x%02X > %s\n", ipv4_hdr->proto,
                   fmt_ipproto(ipv4_hdr->proto));
            printf("\t\tChecksum:\t%u\n", ipv4_hdr->csum);
            printf("\t\tSource:\t\t%s\n", ip4_ssaddr);
            printf("\t\tDestination:\t%s\n", ip4_sdaddr);

            if (ipv4_hdr->proto == IP_P_TCP) {
                struct frame *tcp_frame = frame_child_copy(ipv4_frame);
                struct tcp_hdr *tcp_hdr = recv_tcp(tcp_frame);

                char flags[10];
                fmt_tcp_flags(tcp_hdr, flags);

                printf("\t\t==> TCP Packet\n");
                printf("\t\t\tS-Port:\t%u\n", tcp_hdr->sport);
                printf("\t\t\tD-Port:\t%u\n", tcp_hdr->dport);
                // TODO: Check seq/ack numbers, they seem too big
                printf("\t\t\tSeq #:\t%u\n", tcp_hdr->seqn);
                printf("\t\t\tAck #:\t%u\n", tcp_hdr->ackn);
                printf("\t\t\tFlags:\t%s\n", flags);
            }
        }

        free_frame(eth_frame);
    }

    perror("recv error (MSG_PEEK|MSG_TRUNC)");

    return 0;
}
