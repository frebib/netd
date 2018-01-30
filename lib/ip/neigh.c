#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include <netstack/eth/arp.h>
#include <netstack/ip/route.h>
#include <netstack/ip/ipv4.h>
#include <netstack/ip/neigh.h>


int neigh_send(struct frame *frame, uint8_t proto, uint16_t flags,
               addr_t *daddr, addr_t *saddr, bool wait) {

    // TODO: Take source address into route calculation

    LOGFN(LVERB, "Finding route to %s", straddr(daddr));
    struct route_entry *rt = route_lookup(daddr);
    if (!rt) {
        // If no route found, return DESTUNREACHABLE error
        LOG(LNTCE, "No route to %s found", straddr(daddr));
        return -EHOSTUNREACH;
    }

    struct log_trans t = LOG_TRANS(LTRCE);
    LOGT(&t, "Route found:  daddr %s", straddr(&rt->daddr));
    LOGT(&t, " gwaddr %s", straddr(&rt->gwaddr));
    LOGT(&t, " nmask %s", straddr(&rt->netmask));
    LOGT_COMMITFN(&t);

    // TODO: Perform correct route/hardware address lookups when appropriate
    if (rt->intf == NULL) {
        LOG(LERR, "Route interface is null for %p", (void *) rt);
        return -EINVAL;
    }

    // Set frame interface now it is known from route
    struct intf *intf = rt->intf;
    frame->intf = intf;

    // If route is a gateway, send to the gateway, otherwise send directly
    addr_t *nexthop = (rt->flags & RT_GATEWAY) ? &rt->gwaddr : daddr;

    if (saddr) {
        // Ensure the saddr passed is valid for the sending interface
        if (!intf_has_addr(intf, saddr)) {
            LOG(LERR, "The requested address %s is invalid for "
                    "interface %s", straddr(saddr), intf->name);

            return -EADDRNOTAVAIL;
        }
    } else {

        // Find the default address of the interface matching daddr->proto
        addr_t def_addr = {.proto = daddr->proto};
        if (!intf_get_addr(intf, &def_addr)) {
            LOG(LERR, "Could not get interface address for %s", intf->name);
            return -EADDRNOTAVAIL;
        }
        if (def_addr.address == 0) {
            LOG(LERR, "Interface %s has no address for IPv4", intf->name);
            return -EADDRNOTAVAIL;
        }

        // Set source-address to address obtained from rt->intf
        saddr = &def_addr;
    }

    // Don't assume ARP. IPv6 uses NDP for neighbour discovery
    switch (daddr->proto) {
        case PROTO_IPV4:
            LOGFN(LTRCE, "Finding %s addr for nexthop: %s",
                  strproto(nexthop->proto), straddr(nexthop));

            struct arp_entry *entry;
            entry = arp_get_entry(&intf->arptbl, intf->proto, nexthop);

            if (entry != NULL) {
                // Route and hardware address obtained, send the packet and ret
                return ipv4_send(frame, proto, flags, daddr->ipv4, saddr->ipv4,
                                 &entry->hwaddr);
            }

            // No existing ARP entry found. Request one
            frame_incref(frame);

            // Enqueue the outgoing packet and request the hardware address
            struct queued_pkt *pending = malloc(sizeof(struct queued_pkt));
            pending->retwait = (retlock_t) RETLOCK_INITIALISER;
            pending->frame = frame;
            pending->saddr = *saddr;
            pending->daddr = *daddr;
            pending->nexthop = *nexthop;
            pending->proto = proto;
            pending->flags = flags;

            // Lock retlock atomically with respect to 'pending'
            retlock_lock(&pending->retwait);
            llist_append(&intf->neigh_outqueue, pending);

            struct timespec timeout = {.tv_sec = ARP_WAIT_TIMEOUT};
            LOGFN(LDBUG, "Requesting hwaddr for %s, (wait %lds)",
                  straddr(nexthop), timeout.tv_sec);

            // TODO: Rate limit ARP requests to prevent flooding

            // Convert proto_t value to ARP_HW_* for transmission
            int err = arp_send_req(intf, arp_proto_hw(intf->proto), saddr,nexthop);
            if (err) {
                LOGE(LNTCE, "arp_send_req");
                return err;
            }

            // TODO: Wait if set as socket option
            // TODO: Remove entry from list on a timer if not waiting

            int ret = 0;
            struct timespec to = { .tv_sec = ARP_WAIT_TIMEOUT };

            if (wait) {

                // Wait for packet to be sent, or timeout to occur
                LOGFN(LINFO, "Waiting for hwaddr to resolve %s", straddr(nexthop));
                err = retlock_timedwait_nolock(&pending->retwait, &to, &ret);

                // Handle timeout
                if (err == ETIMEDOUT)
                    ret = -EHOSTUNREACH;
                // Handle other generic errors
                else if (err)
                    ret = -err;

                // Deallocate dynamic memory
                free(pending);
            } else {
                // TODO: Set a timer to remove after a certain amount of time
                retlock_unlock(&pending->retwait);
            }

            return ret;

        default:
            return -EPROTONOSUPPORT;
    }
}

void neigh_update_hwaddr(struct intf *intf, addr_t *daddr, addr_t *hwaddr) {
    pthread_mutex_lock(&intf->neigh_outqueue.lock);

    LOG(LVERB, "Attempting to process %zu queued packets for %s",
        intf->neigh_outqueue.length, intf->name);

    for_each_llist(&intf->neigh_outqueue) {
        struct queued_pkt *tosend = llist_elem_data();

        // Lock the tosend entry to check address
        retlock_lock(&tosend->retwait);

        // Found entry to update and send
        if (addreq(&tosend->nexthop, daddr)) {

            struct log_trans trans = LOG_TRANS(LDBUG);
            LOGT(&trans, "Sending queued packet to %s",
                 straddr(&tosend->nexthop));
            LOGT(&trans, " with hwaddr %s", straddr(hwaddr));
            LOGT_COMMIT(&trans);

            // Remove queued packet from queue and unlock list
            llist_remove_nolock(&intf->neigh_outqueue, tosend);
            pthread_mutex_unlock(&intf->neigh_outqueue.lock);

            // Send the queued frame!
            int ret = ipv4_send(tosend->frame, tosend->proto, tosend->flags,
                             tosend->daddr.ipv4, tosend->saddr.ipv4, hwaddr);

            // Signal any waiting threads with the return value
            retlock_signal_nolock(&tosend->retwait, ret);
            return;
        }
        // Unlock regardless and continue
        retlock_unlock(&tosend->retwait);
    }

    pthread_mutex_unlock(&intf->neigh_outqueue.lock);
}