/*
 * web_socket.h — the browser's sockets, for the web build only (Emscripten).
 *
 * A browser tab can open no TCP and no UDP. The web build reaches RPCN through
 * the gateway (web/gateway/, WEB-NETPLAY.md section 4) over two kinds of
 * WebSocket, and this file is the whole of the C side of that:
 *
 *   stream   <gateway>/stream: the bytes of one TLS session to RPCN. tls.h's web
 *            backend sits on it. The browser does the TLS to the gateway and
 *            checks its certificate; the gateway does the TLS to RPCN.
 *   datagram <gateway>/dgram: one message per datagram, framed
 *            [ip: 4, network order][port: u16 BE][payload]. net_socket.h's UDP
 *            functions sit on it.
 *
 * NOTHING BLOCKS. A socket is usable the moment it is opened: sends made while
 * it is still connecting are queued in JavaScript and go out on open, and an
 * open that fails reads as a socket that closed, with the reason in
 * m2ws_error. That is what lets the netcode stay a polled state machine on the
 * one thread the web build has (rpcn_poll: "Never blocks").
 *
 * Received messages wait in a JavaScript queue until C polls for them, so the
 * queue holds copies and no view of the wasm heap is kept across calls (heap
 * growth replaces the views).
 */
#ifndef WEB_SOCKET_H
#define WEB_SOCKET_H

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Where the gateway is. The page may change it (?gw=, for a local gateway)
 * before anything connects: web_netplay_set_gateway in main_web.c. */
static char g_web_gateway_url[256] = "wss://rpcn.sonicthefighte.rs/gw";

/* The address signaling is sent to, network byte order. A browser cannot resolve
 * a name, so the gateway recognises this constant and forwards to the real
 * helper, and labels the helper's replies with it. Must match SIGNALING_TAG in
 * web/gateway/rules.mjs: 100.127.255.254. */
#define WEB_SIGNALING_TAG_BYTES { 100, 127, 255, 254 }

enum { M2WS_CONNECTING = 0, M2WS_OPEN = 1, M2WS_CLOSED = 2 };

/*
 * Opens a socket and returns its id (>= 1). `datagram` selects message framing
 * for the receive side and caps the queue, since a datagram channel that nobody
 * reads should drop, not grow.
 *
 * A datagram socket also answers the page's round-trip probe: a frame addressed
 * to 0.0.0.0:0 comes straight back from the gateway, and is consumed here rather
 * than delivered to C (Module.m2wsPing in m2hle-netplay.js sends them).
 */
EM_JS(int, m2ws_open, (const char *url_ptr, int datagram), {
    const M = Module;
    if (!M.m2ws) M.m2ws = { next: 1, socks: {} };
    const id = M.m2ws.next++;
    const s = { ws: null, q: [], off: 0, state: 0, opened: false, error: "", out: [], outBytes: 0,
                dgram: !!datagram, rtt: [] };
    M.m2ws.socks[id] = s;
    const url = UTF8ToString(url_ptr);
    try {
        s.ws = new WebSocket(url);
    } catch (e) {
        s.state = 2;
        s.error = 'could not open ' + url + ': ' + (e && e.message ? e.message : e);
        return id;
    }
    s.ws.binaryType = 'arraybuffer';
    s.ws.onopen = () => {
        s.state = 1;
        s.opened = true;
        for (const b of s.out) s.ws.send(b);
        s.out = [];
        s.outBytes = 0;
    };
    s.ws.onmessage = (e) => {
        if (!(e.data instanceof ArrayBuffer)) return;
        const b = new Uint8Array(e.data);
        if (s.dgram) {
            if (b.length === 14 && b[0] === 0 && b[1] === 0 && b[2] === 0 && b[3] === 0 && b[4] === 0 && b[5] === 0) {
                const sent = new DataView(e.data).getFloat64(6);
                s.rtt.push(performance.now() - sent);
                if (s.rtt.length > 16) s.rtt.shift();
                return;
            }
            if (s.q.length >= 512) return;
        }
        s.q.push(b);
    };
    s.ws.onclose = (e) => {
        if (s.state === 2) return;
        s.state = 2;
        if (!s.error) {
            s.error = e.reason ? 'the gateway: ' + e.reason
                    : s.opened ? 'the connection to the gateway closed'
                    : 'the gateway (' + url + ') could not be reached';
        }
    };
    s.ws.onerror = () => {};
    return id;
});

/* 0 connecting, 1 open, 2 closed (or never existed). */
EM_JS(int, m2ws_state, (int id), {
    const s = Module.m2ws && Module.m2ws.socks[id];
    return s ? s.state : 2;
});

/* Queues or sends one message. 0 if the socket is closed or the pre-open queue
 * is full. */
EM_JS(int, m2ws_send, (int id, const void *ptr, int len), {
    const s = Module.m2ws && Module.m2ws.socks[id];
    if (!s || s.state === 2) return 0;
    const b = HEAPU8.slice(ptr, ptr + len);
    if (s.state === 0) {
        if (s.outBytes + len > 262144) return 0;
        s.out.push(b);
        s.outBytes += len;
        return 1;
    }
    try { s.ws.send(b); } catch (e) { return 0; }
    return 1;
});

/* Stream read: up to `cap` bytes across message boundaries. Returns the count,
 * 0 for nothing yet, -1 once the socket has closed and everything that arrived
 * before the close has been read. */
EM_JS(int, m2ws_recv_stream, (int id, void *ptr, int cap), {
    const s = Module.m2ws && Module.m2ws.socks[id];
    if (!s) return -1;
    let n = 0;
    while (n < cap && s.q.length) {
        const head = s.q[0];
        const take = Math.min(cap - n, head.length - s.off);
        HEAPU8.set(head.subarray(s.off, s.off + take), ptr + n);
        n += take;
        s.off += take;
        if (s.off === head.length) { s.q.shift(); s.off = 0; }
    }
    if (n === 0 && s.state === 2) return -1;
    return n;
});

/* Datagram read: one whole message, or 0. A message larger than `cap` is
 * dropped, as a UDP socket would truncate it past use. */
EM_JS(int, m2ws_recv_msg, (int id, void *ptr, int cap), {
    const s = Module.m2ws && Module.m2ws.socks[id];
    if (!s) return 0;
    while (s.q.length) {
        const b = s.q.shift();
        if (b.length > cap) continue;
        HEAPU8.set(b, ptr);
        return b.length;
    }
    return 0;
});

EM_JS(void, m2ws_error, (int id, char *out, int cap), {
    const s = Module.m2ws && Module.m2ws.socks[id];
    stringToUTF8(s ? (s.error || "") : 'no such connection', out, cap);
});

EM_JS(void, m2ws_close, (int id), {
    const s = Module.m2ws && Module.m2ws.socks[id];
    if (!s) return;
    delete Module.m2ws.socks[id];
    if (s.ws && s.state !== 2) {
        s.state = 2;
        try { s.ws.close(1000); } catch (e) {}
    }
});

/* "<gateway>/<path>" into `out`. */
static inline void m2ws_url(char *out, size_t cap, const char *path) {
    size_t n = strlen(g_web_gateway_url);
    while (n && g_web_gateway_url[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/%s", (int)n, g_web_gateway_url, path);
}

#endif /* WEB_SOCKET_H */
