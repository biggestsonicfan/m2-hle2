/*
 * tls.h — the TLS client RPCN's session runs over, behind a one-backend seam.
 *
 * RPCN listens on TCP 31313 and speaks TLS before a single byte of its own
 * protocol. There is no plaintext mode to fall back on, so this is not optional
 * plumbing: without it nothing in net/ can reach a server at all.
 *
 * BACKENDS. Windows uses Schannel (SSPI), which ships with the OS — so netplay
 * adds no vendored crypto library and no submodule to a tree that deliberately
 * has neither, and links only secur32/crypt32/bcrypt. Everywhere else this
 * compiles to a stub that fails the connect with a message saying so, which
 * keeps the rest of net/ building and running on POSIX (the lockstep engine, the
 * packet formats and the LAN/direct path are all platform-agnostic) while
 * leaving exactly one file to write for an OpenSSL backend later. The web build
 * (Emscripten) has a third backend that is not TLS at all: a WebSocket to the
 * gateway, which holds the TLS session to RPCN (see that section below). The seam is
 * the tls_client_t API below and nothing else: no other module knows Schannel
 * exists.
 *
 * TWO WAYS TO TRUST A SERVER, chosen by whether a fingerprint was configured —
 * this part is not a detail, it is the difference between "cannot connect" and
 * "connects to the wrong thing":
 *
 *   PINNED (fingerprint given). RPCN's own --cert-gen produces a SELF-SIGNED
 *   certificate with CN="RPCN" and no subjectAltName. Ordinary validation can
 *   never pass against that, however the server is reached — there is nothing to
 *   match and no chain to build. The certificate's SHA-256 is compared against
 *   the configured value instead, which for one known server is a stronger
 *   statement than trusting the whole public CA set.
 *
 *   VALIDATED (no fingerprint). Full chain + host name validation against the
 *   system trust store, as any HTTPS client does. This is the mode for a server
 *   with a real certificate on a real domain, and it is why pinning must NOT be
 *   used there: a publicly issued certificate is reissued at every renewal (~60
 *   days for Let's Encrypt) and its fingerprint changes with it, so a pin would
 *   break every client a couple of months later.
 *
 * Either way the fingerprint the server presented is recorded and named in the
 * error text, so adopting a self-signed server is: connect once, read the
 * fingerprint out of the refusal, paste it into the pin box.
 *
 * Ported from yampnet's TlsClient.cpp (RipleyTom/rpcn's client, as used by YAMP).
 */
#ifndef TLS_H
#define TLS_H

#include "net_socket.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- Certificate fingerprint --------------------------------------------- */

typedef struct {
    uint8_t bytes[32];
    bool    is_set;
} cert_fingerprint_t;

static inline int tls_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 64 hex characters, optionally ':'/' '/'-' separated. */
static inline bool cert_fp_from_hex(cert_fingerprint_t *fp, const char *text) {
    memset(fp, 0, sizeof(*fp));
    if (!text) return false;
    uint32_t n = 0;
    int hi = -1;
    for (const char *p = text; *p; p++) {
        if (*p == ':' || *p == ' ' || *p == '-') continue;
        int v = tls_hex_val(*p);
        if (v < 0) return false;
        if (hi < 0) { hi = v; continue; }
        if (n >= sizeof(fp->bytes)) return false;
        fp->bytes[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0 || n != sizeof(fp->bytes)) return false;
    fp->is_set = true;
    return true;
}

static inline void cert_fp_to_hex(const cert_fingerprint_t *fp, char *out, uint32_t cap) {
    static const char kHex[] = "0123456789ABCDEF";
    if (!out || cap < 2 * sizeof(fp->bytes) + 1) { if (out && cap) out[0] = '\0'; return; }
    for (uint32_t i = 0; i < sizeof(fp->bytes); i++) {
        out[i * 2]     = kHex[fp->bytes[i] >> 4];
        out[i * 2 + 1] = kHex[fp->bytes[i] & 0xF];
    }
    out[sizeof(fp->bytes) * 2] = '\0';
}

/* ---- Client -------------------------------------------------------------- */

#define TLS_IO_BUFFER (32 * 1024)

typedef struct {
    net_sock_t sock;
    bool       connected;
    bool       have_ctx;
    bool       lib_held;
    /* The peer sent close_notify (or the TCP socket closed). Plaintext already
     * decrypted must still be delivered BEFORE the close is reported: RPCN
     * answers and hangs up in the same breath on some errors, and dropping that
     * reply loses the reason it hung up. */
    bool       peer_closed;

    /* Schannel stream sizes, learned after the handshake. */
    uint32_t   header_size;
    uint32_t   trailer_size;
    uint32_t   max_message;

    /* Encrypted bytes read but not yet decrypted, and plaintext decrypted but
     * not yet handed out. TLS is record-oriented; callers are not. */
    uint8_t   *enc;
    uint32_t   enc_used, enc_cap;
    uint8_t   *plain;
    uint32_t   plain_used, plain_off, plain_cap;

    void      *cred;   /* CredHandle*  — void so the header stays platform-free */
    void      *ctx;    /* CtxtHandle*  */

    cert_fingerprint_t server_fp;
    char      error[256];
} tls_client_t;

static inline void tls_fail(tls_client_t *t, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(t->error, sizeof(t->error), fmt, args);
    va_end(args);
}

static inline const char *tls_last_error(const tls_client_t *t) { return t->error; }
static inline bool tls_is_connected(const tls_client_t *t) { return t->connected; }

/* ======================================================================== */
#ifdef _WIN32
/* ======================================================================== */

#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#include <sspi.h>
#include <wincrypt.h>
#include <bcrypt.h>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

static inline void tls_close(tls_client_t *t) {
    if (t->have_ctx && t->ctx) { DeleteSecurityContext((CtxtHandle *)t->ctx); t->have_ctx = false; }
    if (t->ctx)  { free(t->ctx);  t->ctx  = NULL; }
    if (t->cred) { FreeCredentialsHandle((CredHandle *)t->cred); free(t->cred); t->cred = NULL; }
    net_close(&t->sock);
    if (t->lib_held) { net_shutdown_lib(); t->lib_held = false; }
    free(t->enc);   t->enc = NULL;   t->enc_used = t->enc_cap = 0;
    free(t->plain); t->plain = NULL; t->plain_used = t->plain_off = t->plain_cap = 0;
    t->connected = false;
    t->peer_closed = false;
}

static inline bool tls_handshake(tls_client_t *t, const char *host) {
    CredHandle *cred = (CredHandle *)t->cred;
    CtxtHandle *ctx  = (CtxtHandle *)t->ctx;

    const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT
                    | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    SECURITY_STATUS ss = SEC_I_CONTINUE_NEEDED;
    bool first = true;
    t->enc_used = 0;

    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        /* Schannel asks for more bytes until it holds a complete flight. */
        if (!first && (ss == SEC_E_INCOMPLETE_MESSAGE || t->enc_used == 0)) {
            if (t->enc_used == t->enc_cap) { tls_fail(t, "handshake buffer overflow"); return false; }
            int got = net_tcp_recv_timeout(t->sock, t->enc + t->enc_used,
                                           t->enc_cap - t->enc_used, 10000);
            if (got <= 0) { tls_fail(t, "connection closed during the TLS handshake"); return false; }
            t->enc_used += (uint32_t)got;
        }

        SecBuffer in[2];
        memset(in, 0, sizeof(in));
        in[0].BufferType = SECBUFFER_TOKEN;
        in[0].pvBuffer   = t->enc;
        in[0].cbBuffer   = t->enc_used;
        in[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc in_desc = { SECBUFFER_VERSION, 2, in };

        SecBuffer out[1];
        memset(out, 0, sizeof(out));
        out[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc out_desc = { SECBUFFER_VERSION, 1, out };

        DWORD attrs = 0;
        TimeStamp expiry;
        memset(&expiry, 0, sizeof(expiry));
        ss = InitializeSecurityContextA(cred,
                                        first ? NULL : ctx,
                                        first ? (SEC_CHAR *)host : NULL,
                                        req, 0, 0,
                                        first ? NULL : &in_desc,
                                        0, ctx, &out_desc, &attrs, &expiry);
        if (first) { first = false; t->have_ctx = true; }

        if (out[0].pvBuffer && out[0].cbBuffer) {
            bool sent = net_tcp_send_all(t->sock, out[0].pvBuffer, out[0].cbBuffer);
            FreeContextBuffer(out[0].pvBuffer);
            if (!sent) { tls_fail(t, "send failed during the TLS handshake"); return false; }
        }

        if (ss == SEC_E_INCOMPLETE_MESSAGE) continue;   /* need more; keep what we have */

        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            /* Anything Schannel did not consume starts the next flight (or the
             * application data) and must be preserved. */
            if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer) {
                memmove(t->enc, t->enc + (t->enc_used - in[1].cbBuffer), in[1].cbBuffer);
                t->enc_used = in[1].cbBuffer;
            } else {
                t->enc_used = 0;
            }
            if (ss == SEC_E_OK) break;
        } else {
            tls_fail(t, "TLS handshake failed (0x%08lX)", (unsigned long)ss);
            return false;
        }
    }

    SecPkgContext_StreamSizes sizes;
    memset(&sizes, 0, sizeof(sizes));
    if (QueryContextAttributes(ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
        tls_fail(t, "could not query TLS stream sizes");
        return false;
    }
    t->header_size  = sizes.cbHeader;
    t->trailer_size = sizes.cbTrailer;
    t->max_message  = sizes.cbMaximumMessage;

    t->plain_cap = t->max_message + t->header_size + t->trailer_size + TLS_IO_BUFFER;
    t->plain     = (uint8_t *)malloc(t->plain_cap);
    if (!t->plain) { tls_fail(t, "out of memory"); return false; }
    return true;
}

/* Schannel's policy errors are numbers nobody can act on. These are the ones a
 * game client actually meets, in the words of what has to be fixed. */
static inline const char *tls_explain_policy(DWORD err) {
    switch (err) {
        case CERT_E_EXPIRED:
        case CERT_E_VALIDITYPERIODNESTING:
            return "the server certificate has expired";
        case CERT_E_UNTRUSTEDROOT:
        case CERT_E_UNTRUSTEDTESTROOT:
            return "the server certificate is not signed by a trusted authority "
                   "(a self-signed server needs its fingerprint pinned instead)";
        case CERT_E_CN_NO_MATCH:
            return "the server certificate is not valid for this host name "
                   "(connect by the name on the certificate, or pin the fingerprint)";
        case CERT_E_CHAINING:
            return "incomplete certificate chain - the server is not sending its "
                   "intermediate certificate";
        case CERT_E_WRONG_USAGE:  return "the server certificate is not valid for server authentication";
        case CERT_E_REVOKED:      return "the server certificate has been revoked";
        case TRUST_E_CERT_SIGNATURE: return "the server certificate's signature is invalid";
        default:                  return "the server certificate could not be validated";
    }
}

static inline bool tls_verify_chain(tls_client_t *t, PCCERT_CONTEXT cert, const char *host) {
    CERT_CHAIN_PARA chain_para;
    memset(&chain_para, 0, sizeof(chain_para));
    chain_para.cbSize = sizeof(chain_para);
    LPSTR usage[] = { (LPSTR)szOID_PKIX_KP_SERVER_AUTH };
    chain_para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    chain_para.RequestedUsage.Usage.cUsageIdentifier = 1;
    chain_para.RequestedUsage.Usage.rgpszUsageIdentifier = usage;

    /* hAdditionalStore = the certificates the server itself sent: that is where
     * the intermediate lives, and without it the chain stops at the leaf and
     * every publicly issued certificate looks untrusted.
     *
     * No revocation flags on purpose. Checking means an OCSP/CRL fetch on the
     * connect path, which fails whenever that responder is slow or unreachable —
     * a client that cannot join a match because someone's OCSP responder is down
     * is worse than one that does not notice a revoked certificate. */
    PCCERT_CHAIN_CONTEXT chain = NULL;
    if (!CertGetCertificateChain(NULL, cert, NULL, cert->hCertStore, &chain_para, 0, NULL, &chain)) {
        tls_fail(t, "could not build a certificate chain (error %lu)", GetLastError());
        return false;
    }

    wchar_t whost[256];
    if (MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, (int)(sizeof(whost) / sizeof(whost[0]))) == 0) {
        CertFreeCertificateChain(chain);
        tls_fail(t, "host name is not usable for certificate validation");
        return false;
    }

    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para;
    memset(&ssl_para, 0, sizeof(ssl_para));
    ssl_para.cbSize        = sizeof(ssl_para);
    ssl_para.dwAuthType    = AUTHTYPE_SERVER;
    ssl_para.pwszServerName = whost;

    CERT_CHAIN_POLICY_PARA policy_para;
    memset(&policy_para, 0, sizeof(policy_para));
    policy_para.cbSize = sizeof(policy_para);
    policy_para.pvExtraPolicyPara = &ssl_para;

    CERT_CHAIN_POLICY_STATUS status;
    memset(&status, 0, sizeof(status));
    status.cbSize = sizeof(status);

    BOOL checked = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                                    &policy_para, &status);
    CertFreeCertificateChain(chain);

    if (!checked) {
        tls_fail(t, "certificate policy check failed (error %lu)", GetLastError());
        return false;
    }
    if (status.dwError != 0) {
        /* The fingerprint goes in the message because this is also how a
         * self-signed server is adopted: the connection is refused, but the
         * value to pin is right there in the refusal. */
        char got[80];
        cert_fp_to_hex(&t->server_fp, got, sizeof(got));
        tls_fail(t, "%s; server presented SHA-256 %s", tls_explain_policy(status.dwError), got);
        return false;
    }
    return true;
}

static inline bool tls_verify_certificate(tls_client_t *t, const char *host,
                                          const cert_fingerprint_t *pinned) {
    PCCERT_CONTEXT cert = NULL;
    if (QueryContextAttributes((CtxtHandle *)t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK
        || !cert) {
        tls_fail(t, "the server presented no certificate");
        return false;
    }

    DWORD hash_len = (DWORD)sizeof(t->server_fp.bytes);
    BOOL hashed = CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, NULL,
                                        cert->pbCertEncoded, cert->cbCertEncoded,
                                        t->server_fp.bytes, &hash_len);
    if (!hashed || hash_len != sizeof(t->server_fp.bytes)) {
        CertFreeCertificateContext(cert);
        tls_fail(t, "could not hash the server certificate");
        return false;
    }
    t->server_fp.is_set = true;

    bool ok;
    if (pinned && pinned->is_set) {
        ok = memcmp(pinned->bytes, t->server_fp.bytes, sizeof(pinned->bytes)) == 0;
        if (!ok) {
            char got[80];
            cert_fp_to_hex(&t->server_fp, got, sizeof(got));
            tls_fail(t, "certificate fingerprint mismatch (the server presented %s)", got);
        }
    } else {
        ok = tls_verify_chain(t, cert, host);
    }

    CertFreeCertificateContext(cert);
    return ok;
}

static inline bool tls_connect(tls_client_t *t, const char *host, uint16_t port,
                               const cert_fingerprint_t *pinned) {
    memset(t, 0, sizeof(*t));
    t->sock = NET_SOCK_INVALID;

    if (!host || !*host) { tls_fail(t, "no server given"); return false; }
    if (!net_startup())  { tls_fail(t, "WSAStartup failed"); return false; }
    t->lib_held = true;

    if (!net_tcp_connect(&t->sock, host, port, 5000)) {
        tls_fail(t, "could not connect to %s:%u", host, port);
        tls_close(t);
        return false;
    }

    /* MANUAL_CRED_VALIDATION: RPCN's self-signed certificate has no SAN and
     * CN="RPCN", so Schannel's own validation could never pass. We verify by
     * fingerprint or by an explicit chain check instead — doing it ourselves is
     * what lets a failure say which of the two modes was in force. */
    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags   = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS
                 | SCH_CRED_NO_SERVERNAME_CHECK;

    t->cred = calloc(1, sizeof(CredHandle));
    t->ctx  = calloc(1, sizeof(CtxtHandle));
    if (!t->cred || !t->ctx) { tls_fail(t, "out of memory"); tls_close(t); return false; }

    TimeStamp expiry;
    memset(&expiry, 0, sizeof(expiry));
    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A,
                                                   SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL,
                                                   (CredHandle *)t->cred, &expiry);
    if (ss != SEC_E_OK) {
        tls_fail(t, "AcquireCredentialsHandle failed (0x%08lX)", (unsigned long)ss);
        tls_close(t);
        return false;
    }

    t->enc_cap = TLS_IO_BUFFER;
    t->enc     = (uint8_t *)malloc(t->enc_cap);
    if (!t->enc) { tls_fail(t, "out of memory"); tls_close(t); return false; }

    if (!tls_handshake(t, host))                   { tls_close(t); return false; }
    if (!tls_verify_certificate(t, host, pinned))  { tls_close(t); return false; }

    t->connected = true;
    return true;
}

static inline bool tls_send_all(tls_client_t *t, const void *data, uint32_t len) {
    if (!t->connected) return false;
    CtxtHandle *ctx = (CtxtHandle *)t->ctx;
    const uint8_t *src = (const uint8_t *)data;

    while (len) {
        const uint32_t chunk = (len < t->max_message) ? len : t->max_message;
        const uint32_t total = t->header_size + chunk + t->trailer_size;
        uint8_t *rec = (uint8_t *)malloc(total);
        if (!rec) { tls_fail(t, "out of memory"); return false; }
        memcpy(rec + t->header_size, src, chunk);

        SecBuffer bufs[4];
        memset(bufs, 0, sizeof(bufs));
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer   = rec;
        bufs[0].cbBuffer   = t->header_size;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer   = rec + t->header_size;
        bufs[1].cbBuffer   = chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer   = rec + t->header_size + chunk;
        bufs[2].cbBuffer   = t->trailer_size;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

        SECURITY_STATUS ss = EncryptMessage(ctx, 0, &desc, 0);
        if (ss != SEC_E_OK) {
            free(rec);
            tls_fail(t, "EncryptMessage failed (0x%08lX)", (unsigned long)ss);
            return false;
        }

        uint32_t out_len = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        bool sent = net_tcp_send_all(t->sock, rec, out_len);
        free(rec);
        if (!sent) { tls_fail(t, "send failed"); return false; }

        src += chunk;
        len -= chunk;
    }
    return true;
}

/* Decrypts whatever complete records are sitting in `enc` into `plain`. */
static inline bool tls_decrypt_pending(tls_client_t *t) {
    CtxtHandle *ctx = (CtxtHandle *)t->ctx;

    while (t->enc_used) {
        SecBuffer bufs[4];
        memset(bufs, 0, sizeof(bufs));
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer   = t->enc;
        bufs[0].cbBuffer   = t->enc_used;
        bufs[1].BufferType = SECBUFFER_EMPTY;
        bufs[2].BufferType = SECBUFFER_EMPTY;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

        SECURITY_STATUS ss = DecryptMessage(ctx, &desc, 0, NULL);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) return true;    /* wait for more bytes */
        if (ss == SEC_I_CONTEXT_EXPIRED) {
            /* Graceful TLS shutdown. Not an error yet: deliver what we hold. */
            t->peer_closed = true;
            t->enc_used = 0;
            return true;
        }
        if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) {
            tls_fail(t, "DecryptMessage failed (0x%08lX)", (unsigned long)ss);
            return false;
        }

        for (int i = 0; i < 4; i++) {
            if (bufs[i].BufferType != SECBUFFER_DATA || !bufs[i].cbBuffer) continue;
            /* Compact consumed plaintext before appending so a long-lived
             * connection does not creep forward through the buffer. */
            if (t->plain_off && t->plain_off == t->plain_used) t->plain_off = t->plain_used = 0;
            if (t->plain_used + bufs[i].cbBuffer > t->plain_cap) {
                tls_fail(t, "plaintext buffer overflow");
                return false;
            }
            memcpy(t->plain + t->plain_used, bufs[i].pvBuffer, bufs[i].cbBuffer);
            t->plain_used += bufs[i].cbBuffer;
        }

        uint32_t extra = 0;
        for (int i = 0; i < 4; i++)
            if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer) extra = bufs[i].cbBuffer;
        if (extra) {
            memmove(t->enc, t->enc + (t->enc_used - extra), extra);
            t->enc_used = extra;
        } else {
            t->enc_used = 0;
        }
    }
    return true;
}

/* Up to `cap` bytes. 0 = nothing available yet (not an error), -1 = closed. */
static inline int tls_recv(tls_client_t *t, void *buf, uint32_t cap) {
    if (!t->connected) return -1;

    for (;;) {
        /* Hand back buffered plaintext first. */
        if (t->plain_off < t->plain_used) {
            uint32_t avail = t->plain_used - t->plain_off;
            uint32_t take  = (avail < cap) ? avail : cap;
            memcpy(buf, t->plain + t->plain_off, take);
            t->plain_off += take;
            if (t->plain_off == t->plain_used) t->plain_off = t->plain_used = 0;
            return (int)take;
        }

        /* Drained; only now does a pending close become the answer. */
        if (t->peer_closed) { tls_fail(t, "the server closed the connection"); return -1; }

        uint32_t pending = 0;
        if (!net_pending(t->sock, &pending)) { tls_fail(t, "socket query failed"); return -1; }
        if (pending == 0) return 0;

        if (t->enc_used == t->enc_cap) { tls_fail(t, "encrypted buffer full"); return -1; }
        int got = (int)recv(t->sock, (char *)(t->enc + t->enc_used),
                            (int)(t->enc_cap - t->enc_used), 0);
        if (got == 0) {
            /* TCP FIN. Decrypt anything still buffered before declaring it over. */
            t->peer_closed = true;
            if (!tls_decrypt_pending(t)) return -1;
            continue;
        }
        if (got < 0) {
            if (net_would_block(net_errno())) return 0;
            tls_fail(t, "recv failed (%d)", net_errno());
            return -1;
        }
        t->enc_used += (uint32_t)got;
        if (!tls_decrypt_pending(t)) return -1;
        /* Loop: deliver whatever that produced, or report "nothing yet". */
    }
}

/* ======================================================================== */
#elif defined(__EMSCRIPTEN__)
/* ======================================================================== */
/*
 * The web build: no TLS here at all. The session is a WebSocket to the gateway
 * (web_socket.h), which the BROWSER encrypts and whose certificate the browser
 * checks against the public CA set -- VALIDATED mode, in effect -- and the
 * gateway holds the TLS session to RPCN, pinned by its own config. So `host`,
 * `port` and `pinned` are not ours to use: the gateway's upstream is fixed on
 * purpose, and a page that could name one would make it an open proxy.
 *
 * The connect never blocks. It returns true at once; sends queue until the
 * socket opens; a gateway that cannot be reached, or that cannot reach RPCN,
 * shows up as the stream closing, with the gateway's reason, on the next read.
 */

static inline void tls_close(tls_client_t *t) {
    net_close(&t->sock);
    t->connected   = false;
    t->peer_closed = false;
}

static inline bool tls_connect(tls_client_t *t, const char *host, uint16_t port,
                               const cert_fingerprint_t *pinned) {
    (void)host; (void)port; (void)pinned;
    memset(t, 0, sizeof(*t));
    char url[300];
    m2ws_url(url, sizeof(url), "stream");
    t->sock = m2ws_open(url, 0);
    if (m2ws_state(t->sock) == M2WS_CLOSED) {
        m2ws_error(t->sock, t->error, (int)sizeof(t->error));
        net_close(&t->sock);
        return false;
    }
    t->connected = true;
    return true;
}

static inline bool tls_send_all(tls_client_t *t, const void *data, uint32_t len) {
    if (!t->connected) { tls_fail(t, "not connected"); return false; }
    if (!m2ws_send(t->sock, data, (int)len)) {
        m2ws_error(t->sock, t->error, (int)sizeof(t->error));
        if (!t->error[0]) tls_fail(t, "the connection to the gateway is not open");
        return false;
    }
    return true;
}

static inline int tls_recv(tls_client_t *t, void *buf, uint32_t cap) {
    if (!t->connected) return -1;
    int got = m2ws_recv_stream(t->sock, buf, (int)cap);
    if (got < 0) {
        m2ws_error(t->sock, t->error, (int)sizeof(t->error));
        t->connected = false;
        return -1;
    }
    return got;
}

/* ======================================================================== */
#else  /* no TLS backend on this platform */
/* ======================================================================== */

static inline void tls_close(tls_client_t *t) {
    net_close(&t->sock);
    if (t->lib_held) { net_shutdown_lib(); t->lib_held = false; }
    t->connected = false;
}

static inline bool tls_connect(tls_client_t *t, const char *host, uint16_t port,
                               const cert_fingerprint_t *pinned) {
    (void)host; (void)port; (void)pinned;
    memset(t, 0, sizeof(*t));
    t->sock = NET_SOCK_INVALID;
    tls_fail(t, "this build has no TLS backend: RPCN's session is TLS-only, and only the "
                "Windows (Schannel) backend is implemented - see src/net/tls.h");
    return false;
}

static inline bool tls_send_all(tls_client_t *t, const void *data, uint32_t len) {
    (void)data; (void)len; (void)t;
    return false;
}

static inline int tls_recv(tls_client_t *t, void *buf, uint32_t cap) {
    (void)buf; (void)cap; (void)t;
    return -1;
}

#endif /* _WIN32 / __EMSCRIPTEN__ */

#endif /* TLS_H */
