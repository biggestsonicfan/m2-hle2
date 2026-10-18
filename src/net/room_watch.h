/*
 * room_watch.h — who came into the room we are in, and who left it, from the
 * netplay snapshot (netplay_status_t), once a frame.
 *
 * Shared by the front-ends that tell the player about it: the libretro core
 * says it as a frontend notification, the desktop build as a toast over the
 * game box (overlay_host_announce). Both used to want the same two rules:
 *
 *  - Walking into a room is not everybody in it "joining": the room's members
 *    as they are for the first second (ROOM_WATCH_SETTLE_FRAMES) are taken as
 *    already there. The room's rows can arrive a poll or two after we do.
 *  - A member row can drop out for a poll while the room is re-read, and that
 *    is not a leave: one has to be missing for that same second running.
 */
#ifndef ROOM_WATCH_H
#define ROOM_WATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "net/netplay.h"

enum { ROOM_WATCH_SETTLE_FRAMES = 60 };

typedef enum { ROOM_WATCH_JOIN, ROOM_WATCH_LEAVE } room_watch_event_t;

typedef void (*room_watch_fn)(room_watch_event_t ev, const char *npid, void *ud);

/* The room's other members as last seen, and how long we have been in it. */
typedef struct {
    struct { uint16_t id; char npid[20]; int missing; } m[ROOM_MAX_MEMBERS];
    uint32_t n;
    uint64_t room;
    int      frames;
} room_watch_t;

/* A member not seen before is a join, unless we have only just come in. */
static inline void room_watch__joins(room_watch_t *w, const netplay_status_t *st, bool fresh,
                                     room_watch_fn fn, void *ud) {
    for (uint32_t i = 0; i < st->member_count; i++) {
        const netplay_member_status_t *m = &st->members[i];
        if (m->is_me || !m->member_id || !m->npid[0]) continue;
        uint32_t k = 0;
        while (k < w->n && w->m[k].id != m->member_id) k++;
        if (k < w->n) { w->m[k].missing = 0; continue; }
        if (w->n >= ROOM_MAX_MEMBERS) continue;
        w->m[w->n].id = m->member_id;
        snprintf(w->m[w->n].npid, sizeof w->m[w->n].npid, "%s", m->npid);
        w->m[w->n].missing = 0;
        w->n++;
        if (!fresh) fn(ROOM_WATCH_JOIN, m->npid, ud);
    }
}

/* A member missing for ROOM_WATCH_SETTLE_FRAMES running is a departure. */
static inline void room_watch__leaves(room_watch_t *w, const netplay_status_t *st,
                                      room_watch_fn fn, void *ud) {
    for (uint32_t k = 0; k < w->n;) {
        bool here = false;
        for (uint32_t i = 0; i < st->member_count; i++)
            if (st->members[i].member_id == w->m[k].id) here = true;
        if (here || ++w->m[k].missing < ROOM_WATCH_SETTLE_FRAMES) { k++; continue; }
        fn(ROOM_WATCH_LEAVE, w->m[k].npid, ud);
        w->m[k] = w->m[--w->n];
    }
}

/* Once a frame, whatever is on screen; fn hears each join and leave. */
static inline void room_watch_step(room_watch_t *w, const netplay_status_t *st,
                                   room_watch_fn fn, void *ud) {
    netplay_state_t s = st->state;
    bool in_room = s == NETPLAY_IN_ROOM || s == NETPLAY_SYNCING ||
                   s == NETPLAY_PLAYING || s == NETPLAY_WATCHING;
    if (!in_room || !st->room_id) {
        w->n = 0;
        w->room = 0;
        return;
    }
    if (st->room_id != w->room) {
        w->n = 0;
        w->room = st->room_id;
        w->frames = 0;
    }
    bool fresh = w->frames < ROOM_WATCH_SETTLE_FRAMES;
    if (fresh) w->frames++;
    room_watch__joins(w, st, fresh, fn, ud);
    room_watch__leaves(w, st, fn, ud);
}

#endif /* ROOM_WATCH_H */
