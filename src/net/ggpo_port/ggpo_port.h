/*
 * ggpo_port.h — what ggpo_port.cpp gives GGPO, and what the emulator can set.
 *
 * GGPO sends and receives through a transport. By default that is a UDP socket
 * of its own, bound to the local port the session names (net_socket.h, so the
 * one Winsock refcount covers it). A host that already owns a socket (the RPCN
 * session's, the web gateway's) can hand GGPO a transport instead, before
 * ggpo_start_session; GGPO then never opens one.
 *
 * C linkage both ways: the emulator is C, GGPO is C++.
 */
#ifndef M2HLE_GGPO_PORT_H
#define M2HLE_GGPO_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ggponet.h spells its callbacks __cdecl, which only MSVC and MinGW know. */
#if !defined(_WIN32) && !defined(__cdecl)
#  define __cdecl
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ggpo_transport_t {
    void *ctx;
    /* ip in network order, port in host order (net_socket.h's convention). */
    bool (*send)(void *ctx, uint32_t ip_be, uint16_t port, const void *data, uint32_t len);
    /* Bytes received, 0 when nothing waits. */
    int  (*recv)(void *ctx, void *buf, uint32_t cap, uint32_t *ip_be, uint16_t *port);
} ggpo_transport_t;

/* NULL goes back to GGPO's own socket. Takes effect at the next session. */
void ggpo_port_set_transport(const ggpo_transport_t *t);

/* The port GGPO's own socket bound, 0 when it has none (failed, or a transport). */
uint16_t ggpo_port_bound(void);

/* Synctest's sync errors, counted and the last one kept (it has no other way
 * to say so: upstream breaks into the debugger). */
uint32_t    ggpo_port_sync_errors(void);
const char *ggpo_port_last_error(void);
void        ggpo_port_reset_errors(void);

/* Upstream's synclogs\ files are off unless this names a directory. */
void ggpo_port_set_synclog_dir(const char *dir);

/* ---- The session, for C ---------------------------------------------------
 *
 * ggponet.h is not C the MSVC compiler takes (an empty struct), so the
 * emulator drives GGPO through these. One session at a time. The return
 * values are GGPO's error codes (0 is success); the events are its event
 * codes, with the player handle and up to two numbers. */

enum {
    GGPO_PORT_OK                 = 0,
    GGPO_PORT_PREDICTION_LIMIT   = 4,
    GGPO_PORT_NOT_SYNCHRONIZED   = 6,
    GGPO_PORT_IN_ROLLBACK        = 7,
};

enum {
    GGPO_PORT_EV_CONNECTED       = 1000,
    GGPO_PORT_EV_SYNCHRONIZING   = 1001,   /* a = count, b = total */
    GGPO_PORT_EV_SYNCHRONIZED    = 1002,
    GGPO_PORT_EV_RUNNING         = 1003,
    GGPO_PORT_EV_DISCONNECTED    = 1004,
    GGPO_PORT_EV_TIMESYNC        = 1005,   /* a = frames ahead */
    GGPO_PORT_EV_INTERRUPTED     = 1006,   /* a = disconnect timeout, ms */
    GGPO_PORT_EV_RESUMED         = 1007,
};

typedef struct ggpo_port_cb_t {
    void *ud;
    /* The board into a new buffer; its length into *len. */
    void *(*save)(void *ud, int *len, int *checksum, int frame);
    bool  (*load)(void *ud, const void *buf, int len);
    void  (*free_buf)(void *ud, void *buf);
    /* A frame again, in a rollback: synchronize, run it, advance. */
    bool  (*advance)(void *ud);
    /* Synctest's mismatch: the two buffers, one call each. */
    void  (*log_state)(void *ud, const char *filename, const void *buf, int len);
    void  (*event)(void *ud, int code, int player, int a, int b);
} ggpo_port_cb_t;

int  ggpo_port_start_p2p(const ggpo_port_cb_t *cb, int input_size, uint16_t local_port,
                         int local_player, const char *remote_ip, uint16_t remote_port);
int  ggpo_port_start_synctest(const ggpo_port_cb_t *cb, int input_size, int frames);
void ggpo_port_close(void);
bool ggpo_port_active(void);
int  ggpo_port_idle(int timeout_ms);
/* which 0: this machine's player; 1: the second local player (synctest). */
int  ggpo_port_add_local_input(int which, const void *values, int size);
int  ggpo_port_sync_input(void *values, int size, int *disconnect_flags);
int  ggpo_port_advance_frame(void);
int  ggpo_port_set_frame_delay(int frames);
int  ggpo_port_set_disconnect_timeout(int ms);
int  ggpo_port_stats(int *ping, int *kbps_sent, int *local_behind, int *remote_behind);

/* For the compat header. */
int  ggpo_port_fopen(FILE **fp, const char *name, const char *mode);
void ggpo_port_debug_text(const char *text);
void ggpo_port_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif
