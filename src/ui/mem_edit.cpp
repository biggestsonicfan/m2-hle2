/*
 * mem_edit.cpp — the C++ side of mem_edit.h.
 *
 * The one exception to the header-only `.h` convention (see CLAUDE.md,
 * Conventions): vendor/imgui_club/imgui_memory_editor.h is a C++ struct that
 * calls the ImGui C++ API directly, so it cannot be included from the C11
 * sources that make up the rest of src/. This file instantiates it and hands
 * out the C handle declared in mem_edit.h; nothing else in src/ sees C++.
 */
#include "imgui.h"
#include "imgui_memory_editor.h"

#include "mem_edit.h"

struct mem_edit {
    MemoryEditor      ed;
    mem_edit_read_fn  rd;
    mem_edit_write_fn wr;
    void             *user;
    uint32_t          base;         /* address of offset 0 of the current draw */
    uint32_t          last_base;
    uint32_t          last_size;
    bool              drawn;
    uint32_t          goto_addr;
    bool              goto_pending;
};

/* MemoryEditor addresses its data by offset from the block it was handed;
 * `base` turns those back into bus addresses for the callbacks. `mem` is the
 * block pointer, which is null here — there is no block. */
static ImU8 mem_edit_read_thunk(const ImU8 *mem, size_t off, void *user_data) {
    (void)mem;
    mem_edit_t *me = (mem_edit_t *)user_data;
    return me->rd ? (ImU8)me->rd(me->user, me->base + (uint32_t)off) : (ImU8)0;
}

static void mem_edit_write_thunk(ImU8 *mem, size_t off, ImU8 d, void *user_data) {
    (void)mem;
    mem_edit_t *me = (mem_edit_t *)user_data;
    if (me->wr) me->wr(me->user, me->base + (uint32_t)off, (uint8_t)d);
}

extern "C" {

mem_edit_t *mem_edit_create(int addr_digits) {
    mem_edit_t *me = new mem_edit();
    me->rd = nullptr;
    me->wr = nullptr;
    me->user = nullptr;
    me->base = 0;
    me->last_base = 0;
    me->last_size = 0;
    me->drawn = false;
    me->goto_addr = 0;
    me->goto_pending = false;

    /* The preview footer replaces the old window's hand-rolled byte/u32/f32
     * readout, and covers every other width and endianness besides. */
    me->ed.OptShowDataPreview = true;
    me->ed.OptAddrDigitsCount = addr_digits;
    me->ed.ReadFn   = mem_edit_read_thunk;
    me->ed.WriteFn  = mem_edit_write_thunk;
    me->ed.UserData = me;
    return me;
}

void mem_edit_set_bus(mem_edit_t *me, mem_edit_read_fn rd, mem_edit_write_fn wr, void *user) {
    me->rd   = rd;
    me->wr   = wr;
    me->user = user;
}

void mem_edit_draw(mem_edit_t *me, uint32_t base, uint32_t size) {
    if (size == 0) { ImGui::TextDisabled("empty region"); return; }
    me->base = base;

    /* The selection is an offset, so it means something else the moment the
     * window moves. Drop it rather than let the preview footer describe a
     * byte the user never clicked. */
    if (!me->drawn || base != me->last_base || size != me->last_size) {
        me->ed.DataPreviewAddr = me->ed.DataEditingAddr = (size_t)-1;
        me->ed.HighlightMin = me->ed.HighlightMax = (size_t)-1;
        me->last_base = base;
        me->last_size = size;
        me->drawn = true;
    }

    if (me->goto_pending) {
        uint64_t off = (uint64_t)me->goto_addr - (uint64_t)base;
        if (me->goto_addr >= base && off < (uint64_t)size)
            me->ed.GotoAddrAndHighlight((size_t)off, (size_t)off + 1);
        me->goto_pending = false;
    }

    me->ed.DrawContents(nullptr, (size_t)size, (size_t)base);
}

void mem_edit_goto(mem_edit_t *me, uint32_t addr) {
    me->goto_addr    = addr;
    me->goto_pending = true;
}

bool mem_edit_selected(mem_edit_t *me, uint32_t *out_addr) {
    if (me->ed.DataPreviewAddr == (size_t)-1) return false;
    if (out_addr) *out_addr = me->base + (uint32_t)me->ed.DataPreviewAddr;
    return true;
}

} /* extern "C" */
