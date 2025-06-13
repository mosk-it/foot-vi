#pragma once

#include <xkbcommon/xkbcommon.h>

#include "key-binding.h"
#include "terminal.h"

void vimode_begin(struct terminal *term);

// vimode_search_begin
//
// Enter search mode directly without needing to interact with the
// vimode first. Enters vimode as well.
//
void vimode_search_begin(struct terminal *term);

void vimode_cancel(struct terminal *term);
void vimode_input(struct seat *seat, struct terminal *term,
                  const struct key_binding_set *bindings, uint32_t key,
                  xkb_keysym_t sym, xkb_mod_mask_t mods,
                  xkb_mod_mask_t consumed, const xkb_keysym_t *raw_syms,
                  size_t raw_count, uint32_t serial);

// vimode_mouse_selection_begin
//
// Enter visual selection mode guided by a mouse. The position of the
// vimode cursor is updated to match the position of the mouse.
//
// Does nothing if vimode is not active.
//
// Parameters:
// point - view-relative position of the mouse.
// vmode - the visual mode to be started. If a non-visual mode is
//         passed, the function does nothing.
//
void vimode_mouse_selection_begin(struct terminal *term, struct coord point,
                                  enum vi_mode vmode);
void vimode_mouse_selection_end(struct terminal *term);
void vimode_mouse_move(struct terminal *term, struct coord point);

void vimode_view_up(struct terminal *term, int new_view);
void vimode_view_down(struct terminal *term, int new_view);

void vimode_search_add_chars(struct terminal *term, const char *text,
                             size_t len);

struct search_match_iterator {
    struct terminal *term;
    struct coord start;
    char32_t const *buf;
    size_t len;
};

struct search_match_iterator search_matches_new_iter(struct terminal *term,
                                                     char32_t const *const buf,
                                                     size_t const len);
struct range search_matches_next(struct search_match_iterator *iter);
