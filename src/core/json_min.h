/*
 * json_min.h — just enough JSON to carry a debug command and its reply.
 *
 * Readers that scan for `"key":` and take what follows, and one escaper for
 * writing a string back. No parse tree, no allocation, no validation: a
 * malformed request reads as a request with fields missing, which is what a
 * debug channel wants — the caller gets a reply saying what it asked for
 * rather than a syntax error.
 *
 * Every reader accepts the value quoted or bare, because both spellings turn
 * up: a caller building the request by hand quotes everything, and one dumping
 * a dictionary does not.
 *
 * Shared by the transports that carry those commands: the TCP bridge
 * (ui/mcp_bridge.h) and the browser build's exported entry points
 * (main_web.c), so a command spelled one way works both ways.
 */
#ifndef JSON_MIN_H
#define JSON_MIN_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write a hex uint32 field. buf must be large enough. */
static inline int json_u32hex(char *buf, int cap, const char *key, uint32_t v) {
    return snprintf(buf, (size_t)cap, "\"%s\":\"0x%08X\"", key, v);
}

/* Where key's value starts (after the colon and any blanks), or NULL. */
static inline const char *json_value_at(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* The string value of `"key":"value"`. Returns 1 on success. */
static inline int json_get_str(const char *json, const char *key, char *out, int out_cap) {
    const char *p = json_value_at(json, key);
    if (!p || *p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* A uint32, decimal or 0x hex, quoted or bare. */
static inline int json_get_u32(const char *json, const char *key, uint32_t *out) {
    char vstr[32];
    if (json_get_str(json, key, vstr, sizeof(vstr))) {
        *out = (uint32_t)strtoul(vstr, NULL, 0);
        return 1;
    }
    const char *p = json_value_at(json, key);
    if (!p) return 0;
    *out = (uint32_t)strtoul(p, NULL, 0);
    return 1;
}

/* A float, quoted or bare. */
static inline int json_get_f32(const char *json, const char *key, float *out) {
    char vstr[48];
    if (json_get_str(json, key, vstr, sizeof(vstr))) { *out = (float)atof(vstr); return 1; }
    const char *p = json_value_at(json, key);
    if (!p) return 0;
    *out = (float)atof(p);
    return 1;
}

/* An int that may arrive as a bool, a bare number or a quoted one. */
static inline int json_get_int(const char *json, const char *key, int *out) {
    char vstr[48];
    if (json_get_str(json, key, vstr, sizeof(vstr))) {
        if (!strcmp(vstr, "true"))  { *out = 1; return 1; }
        if (!strcmp(vstr, "false")) { *out = 0; return 1; }
        *out = (int)strtol(vstr, NULL, 0);
        return 1;
    }
    const char *p = json_value_at(json, key);
    if (!p) return 0;
    if (!strncmp(p, "true", 4))  { *out = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    *out = (int)strtol(p, NULL, 0);
    return 1;
}

/* Escape a string for a JSON value. Returns the length written. */
static inline int json_escape(char *out, int cap, const char *in) {
    int n = 0;
    if (cap <= 0) return 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        char esc[8];
        int len;
        switch (*p) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  len = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; len = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  len = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  len = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  len = 2; break;
            default:
                if (*p < 0x20) len = snprintf(esc, sizeof(esc), "\\u%04X", *p);
                else         { esc[0] = (char)*p; len = 1; }
                break;
        }
        if (n + len >= cap) break;
        memcpy(out + n, esc, (size_t)len);
        n += len;
    }
    out[n] = '\0';          /* the break above keeps n <= cap - 1 */
    return n;
}

#endif /* JSON_MIN_H */
