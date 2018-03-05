#ifndef NETSTACK_UTIL_H
#define NETSTACK_UTIL_H

#include <stdint.h>
#include <time.h>

// Nanoseconds in one second
#define NSPERSEC 1000000000

// Milliseconds in one second
#define MSPERSEC 1000

// Nanoseconds in one millisecond
#define NSPERMS  1000000

// Convert seconds to nanoseconds
#define sectons(sec)    ((sec) * NSPERSEC)

// Convert nanoseconds to seconds
#define nstosec(ns)     ((ns) / NSPERSEC)

// Convert seconds to milliseconds
#define sectoms(sec)    ((sec) * MSPERSEC)

// Convert nanoseconds to seconds
#define mstosec(ms)     ((ms) / MSPERSEC)

// Convert milliseconds to nanoseconds
#define mstons(ms)      ((ms) * NSPERMS)

// Convert nanoseconds to milliseconds
#define nstoms(ns)      ((ns) / NSPERMS)

/*!
 * Adds seconds and nanoseconds into t1, accounting for nanosecond overflow
 */
static void timespecaddp(struct timespec *t1, const time_t sec, const long nsec) {
    t1->tv_nsec += nsec;
    if (t1->tv_nsec >= NSPERSEC) {
        t1->tv_nsec -= NSPERSEC;
        t1->tv_sec += sec + 1;
    } else {
        t1->tv_sec += sec;
    }
}

/*!
 * Adds t2 into t1, accounting for nanosecond overflow
 */
static void timespecadd(struct timespec *t1, const struct timespec *t2) {
    timespecaddp(t1, t2->tv_sec, t2->tv_nsec);
}

#endif //NETSTACK_UTIL_H
