#include <string.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "vimode"
#define LOG_ENABLE_DBG 0
#include "char32.h"
#include "commands.h"
#include "grid.h"
#include "key-binding.h"
#include "log.h"
#include "render.h"
#include "selection.h"
#include "util.h"
#include "vimode.h"
#include "xmalloc.h"

static bool is_mode_visual(enum vi_mode const mode) {
  return mode == VI_MODE_VISUAL || mode == VI_MODE_VLINE ||
         mode == VI_MODE_VBLOCK;
}

static enum selection_kind
selection_kind_from_vi_mode(enum vi_mode const mode) {
  switch (mode) {
  case VI_MODE_VISUAL:
    return SELECTION_CHAR_WISE;
  case VI_MODE_VLINE:
    return SELECTION_LINE_WISE;
  case VI_MODE_VBLOCK:
    return SELECTION_BLOCK;
  default:
    BUG("invalid vi mode");
    return SELECTION_NONE;
  }
}

static struct coord offset_to_view_relative(struct terminal *const term,
                                            struct coord coord) {
  coord.row += term->grid->offset;
  coord.row -= term->grid->view;
  return coord;
}

static struct coord view_to_offset_relative(struct terminal *const term,
                                            struct coord coord) {
  coord.row += term->grid->view;
  coord.row -= term->grid->offset;
  return coord;
}

static void damage_cursor_cell(struct terminal *const term) {
  struct coord const cursor =
      offset_to_view_relative(term, term->vimode.cursor);
  term_damage_cell_in_view(term, cursor.row, cursor.col);
}

static void clip_cursor_to_view(struct terminal *const term) {
  damage_cursor_cell(term);
  printf("CLIP CURSOR BEFORE (%d, %d) [view=%d; offset=%d]\n",
         term->vimode.cursor.row, term->vimode.cursor.col, term->grid->view,
         term->grid->offset);
  struct coord cursor = offset_to_view_relative(term, term->vimode.cursor);
  printf("CLIP CURSOR VIEW RELATIVE BEFORE (%d, %d)\n", cursor.row, cursor.col);
  if (cursor.row < 0) {
    cursor.row = 0;
  } else if (cursor.row >= term->rows) {
    cursor.row = term->rows - 1;
  }
  printf("CLIP CURSOR VIEW RELATIVE AFTER (%d, %d)\n", cursor.row, cursor.col);
  term->vimode.cursor = view_to_offset_relative(term, cursor);
  printf("CLIP CURSOR AFTER (%d, %d)\n", term->vimode.cursor.row,
         term->vimode.cursor.col);
  damage_cursor_cell(term);
  render_refresh(term);
}

static void update_selection(struct seat *const seat,
                             struct terminal *const term) {
  enum vi_mode const mode = term->vimode.mode;
  if (is_mode_visual(mode)) {
    struct coord const cursor = term->grid->cursor.point;
    printf("UPDATING SELECTION [row=%d; col=%d]\n", cursor.row, cursor.col);
    selection_update(term, cursor.col, cursor.row);
  }
}

/*
 * Ensures a "new" viewport doesn't contain any unallocated rows.
 *
 * This is done by first checking if the *first* row is NULL. If so,
 * we move the viewport *forward*, until the first row is non-NULL. At
 * this point, the entire viewport should be allocated rows only.
 *
 * If the first row already was non-NULL, we instead check the *last*
 * row, and if it is NULL, we move the viewport *backward* until the
 * last row is non-NULL.
 */
static int ensure_view_is_allocated(struct terminal *term, int new_view) {
  struct grid *grid = term->grid;
  int view_end = (new_view + term->rows - 1) & (grid->num_rows - 1);

  if (grid->rows[new_view] == NULL) {
    while (grid->rows[new_view] == NULL)
      new_view = (new_view + 1) & (grid->num_rows - 1);
  }

  else if (grid->rows[view_end] == NULL) {
    while (grid->rows[view_end] == NULL) {
      new_view--;
      if (new_view < 0)
        new_view += grid->num_rows;
      view_end = (new_view + term->rows - 1) & (grid->num_rows - 1);
    }
  }

#if defined(_DEBUG)
  for (size_t r = 0; r < term->rows; r++)
    xassert(grid->rows[(new_view + r) & (grid->num_rows - 1)] != NULL);
#endif

  return new_view;
}

static bool search_ensure_size(struct terminal *term, size_t wanted_size) {
  struct vimode_search *const search = &term->vimode.search;
  while (wanted_size >= search->sz) {
    size_t new_sz = search->sz == 0 ? 64 : search->sz * 2;
    char32_t *new_buf = realloc(search->buf, new_sz * sizeof(search->buf[0]));

    if (new_buf == NULL) {
      LOG_ERRNO("failed to resize search buffer");
      return false;
    }

    search->buf = new_buf;
    search->sz = new_sz;
  }

  return true;
}

static void start_search(struct terminal *term) {
  if (term->vimode.is_searching) {
    return;
  }

  LOG_DBG("vimode-search: begin");

  const struct grid *grid = term->grid;
  term->vimode.search.original_view = grid->view;
  term->vimode.search.len = 0;
  term->vimode.search.sz = 64;
  term->vimode.search.buf =
      xmalloc(term->vimode.search.sz * sizeof(term->vimode.search.buf[0]));
  term->vimode.search.buf[0] = U'\0';
  term->vimode.search.direction = SEARCH_FORWARD;
  term->vimode.is_searching = true;

  /* On-demand instantiate wayland surface */
  bool ret =
      wayl_win_subsurface_new(term->window, &term->window->search, false);
  xassert(ret);

  render_refresh_vimode_search_box(term);
}

static void cancel_search(struct terminal *const term) {
  if (!term->vimode.is_searching) {
    return;
  }

  wayl_win_subsurface_destroy(&term->window->search);
  term->vimode.is_searching = false;
  struct vimode_search *const search = &term->vimode.search;
  if (search->buf != NULL) {
    free(search->buf);
    search->buf = NULL;
  }
  search->len = search->sz = 0;
  search->cursor = 0;
  search->original_view = 0;
  search->match = (struct coord){-1, -1};
  search->match_len = 0;
  term->render.search_glyph_offset = 0;

  term->grid->view = ensure_view_is_allocated(term, search->original_view);
  term_damage_view(term);
  render_refresh(term);
}

void vimode_search_begin(struct terminal *term) {
  vimode_begin(term);
  start_search(term);
  term_xcursor_update(term);
}

void vimode_begin(struct terminal *term) {
  LOG_DBG("vimode: begin");
  printf("VIMODE BEGIN [grid rows=%d]\n", term->grid->num_rows);

  vimode_cancel(term);

  term->vimode.cursor = term->grid->cursor.point;
  // From a user's perspective, it is reasonable to expect that the
  // mode will launch at the exact position in the scrollback they are
  // currently viewing, thus we move the cursor into the view.
  clip_cursor_to_view(term);

  /* Reset IME state */
  if (term_ime_is_enabled(term)) {
    term_ime_disable(term);
    term_ime_enable(term);
  }

  term->is_vimming = true;

  term_xcursor_update(term);
}

void vimode_cancel(struct terminal *term) {
  if (!term->is_vimming) {
    return;
  }

  printf("VIMODE CANCEL\n");

  cancel_search(term);

  term->is_vimming = false;

  /* Reset IME state */
  if (term_ime_is_enabled(term)) {
    term_ime_disable(term);
    term_ime_enable(term);
  }

  selection_cancel(term);
  struct grid *const grid = term->grid;
  grid->view = grid->offset;
  term_damage_view(term);
  term_xcursor_update(term);
  render_refresh(term);
}

static ssize_t matches_cell(const struct terminal *term,
                            const struct cell *cell, size_t search_ofs) {
  assert(search_ofs < term->vimode.search.len);

  char32_t base = cell->wc;
  const struct composed *composed = NULL;

  if (base >= CELL_COMB_CHARS_LO && base <= CELL_COMB_CHARS_HI) {
    composed = composed_lookup(term->composed, base - CELL_COMB_CHARS_LO);
    base = composed->chars[0];
  }

  if (composed == NULL && base == 0 &&
      term->vimode.search.buf[search_ofs] == U' ')
    return 1;

  if (c32ncasecmp(&base, &term->vimode.search.buf[search_ofs], 1) != 0)
    return -1;

  if (composed != NULL) {
    if (search_ofs + composed->count > term->vimode.search.len)
      return -1;

    for (size_t j = 1; j < composed->count; j++) {
      if (composed->chars[j] != term->vimode.search.buf[search_ofs + j])
        return -1;
    }
  }

  return composed != NULL ? composed->count : 1;
}

static bool find_next(struct terminal *term, enum search_direction direction,
                      struct coord abs_start, struct coord abs_end,
                      struct range *match) {
#define ROW_DEC(_r) ((_r) = ((_r) - 1 + grid->num_rows) & (grid->num_rows - 1))
#define ROW_INC(_r) ((_r) = ((_r) + 1) & (grid->num_rows - 1))

  struct grid *grid = term->grid;
  const bool backward = direction != SEARCH_FORWARD;

  LOG_DBG("%s: start: %dx%d, end: %dx%d", backward ? "backward" : "forward",
          abs_start.row, abs_start.col, abs_end.row, abs_end.col);

  xassert(abs_start.row >= 0);
  xassert(abs_start.row < grid->num_rows);
  xassert(abs_start.col >= 0);
  xassert(abs_start.col < term->cols);

  xassert(abs_end.row >= 0);
  xassert(abs_end.row < grid->num_rows);
  xassert(abs_end.col >= 0);
  xassert(abs_end.col < term->cols);

  for (int match_start_row = abs_start.row, match_start_col = abs_start.col;;
       backward ? ROW_DEC(match_start_row) : ROW_INC(match_start_row)) {

    const struct row *row = grid->rows[match_start_row];
    if (row == NULL) {
      if (match_start_row == abs_end.row)
        break;
      continue;
    }

    for (; backward ? match_start_col >= 0 : match_start_col < term->cols;
         backward ? match_start_col-- : match_start_col++) {
      if (matches_cell(term, &row->cells[match_start_col], 0) < 0) {
        if (match_start_row == abs_end.row && match_start_col == abs_end.col) {
          break;
        }
        continue;
      }

      /*
       * Got a match on the first letter. Now we'll see if the
       * rest of the search buffer matches.
       */

      LOG_DBG("search: initial match at row=%d, col=%d", match_start_row,
              match_start_col);

      int match_end_row = match_start_row;
      int match_end_col = match_start_col;
      const struct row *match_row = row;
      size_t match_len = 0;

      for (size_t i = 0; i < term->vimode.search.len;) {
        if (match_end_col >= term->cols) {
          ROW_INC(match_end_row);
          match_end_col = 0;

          match_row = grid->rows[match_end_row];
          if (match_row == NULL)
            break;
        }

        if (match_row->cells[match_end_col].wc >= CELL_SPACER) {
          match_end_col++;
          continue;
        }

        ssize_t additional_chars =
            matches_cell(term, &match_row->cells[match_end_col], i);
        if (additional_chars < 0)
          break;

        i += additional_chars;
        match_len += additional_chars;
        match_end_col++;

        while (match_end_col < term->cols &&
               match_row->cells[match_end_col].wc > CELL_SPACER) {
          match_end_col++;
        }
      }

      if (match_len != term->vimode.search.len) {
        /* Didn't match (completely) */

        if (match_start_row == abs_end.row && match_start_col == abs_end.col) {
          break;
        }

        continue;
      }

      *match = (struct range){
          .start = {match_start_col, match_start_row},
          .end = {match_end_col - 1, match_end_row},
      };

      return true;
    }

    if (match_start_row == abs_end.row && match_start_col == abs_end.col)
      break;

    match_start_col = backward ? term->cols - 1 : 0;
  }

  return false;
}

static void search_find_next(struct terminal *term,
                             enum search_direction direction) {
  struct grid *grid = term->grid;

  if (term->vimode.search.len == 0) {
    term->vimode.search.match = (struct coord){-1, -1};
    return;
  }

  struct coord start = term->vimode.search.match;
  size_t len = term->vimode.search.match_len;

  xassert((len == 0 && start.row == -1 && start.col == -1) ||
          (len > 0 && start.row >= 0 && start.col >= 0));

  if (len == 0) {
    /* No previous match, start from the top, or bottom, of the scrollback */
    switch (direction) {
    case SEARCH_FORWARD:
      start.row = grid_row_absolute_in_view(grid, 0);
      start.col = 0;
      break;

    case SEARCH_BACKWARD:
    case SEARCH_BACKWARD_SAME_POSITION:
      start.row = grid_row_absolute_in_view(grid, term->rows - 1);
      start.col = term->cols - 1;
      break;
    }
  } else {
    /* Continue from last match */
    xassert(start.row >= 0);
    xassert(start.col >= 0);

    switch (direction) {
    case SEARCH_BACKWARD_SAME_POSITION:
      break;

    case SEARCH_BACKWARD:
      if (--start.col < 0) {
        start.col = term->cols - 1;
        start.row += grid->num_rows - 1;
        start.row &= grid->num_rows - 1;
      }
      break;

    case SEARCH_FORWARD:
      if (++start.col >= term->cols) {
        start.col = 0;
        start.row++;
        start.row &= grid->num_rows - 1;
      }
      break;
    }

    xassert(start.row >= 0);
    xassert(start.row < grid->num_rows);
    xassert(start.col >= 0);
    xassert(start.col < term->cols);
  }

  LOG_DBG("update: %s: starting at row=%d col=%d "
          "(offset = %d, view = %d)",
          direction != SEARCH_FORWARD ? "backward" : "forward", start.row,
          start.col, grid->offset, grid->view);

  struct coord end = start;
  switch (direction) {
  case SEARCH_FORWARD:
    /* Search forward, until we reach the cell *before* current start */
    if (--end.col < 0) {
      end.col = term->cols - 1;
      end.row += grid->num_rows - 1;
      end.row &= grid->num_rows - 1;
    }
    break;

  case SEARCH_BACKWARD:
  case SEARCH_BACKWARD_SAME_POSITION:
    /* Search backwards, until we reach the cell *after* current start */
    if (++end.col >= term->cols) {
      end.col = 0;
      end.row++;
      end.row &= grid->num_rows - 1;
    }
    break;
  }

  struct range match;
  bool found = find_next(term, direction, start, end, &match);
  if (found) {
    LOG_DBG("primary match found at %dx%d", match.start.row, match.start.col);
    term->vimode.search.match = match.start;
    term->vimode.search.match_len = term->vimode.search.len;
  } else {
    LOG_DBG("no match");
    term->vimode.search.match = (struct coord){-1, -1};
    term->vimode.search.match_len = 0;
  }
#undef ROW_DEC
}

struct search_match_iterator search_matches_new_iter(struct terminal *term) {
  return (struct search_match_iterator){
      .term = term,
      .start = {0, 0},
  };
}

struct range search_matches_next(struct search_match_iterator *iter) {
  struct terminal *term = iter->term;
  struct grid *grid = term->grid;

  if (term->vimode.search.match_len == 0)
    goto no_match;

  if (iter->start.row >= term->rows)
    goto no_match;

  xassert(iter->start.row >= 0);
  xassert(iter->start.row < term->rows);
  xassert(iter->start.col >= 0);
  xassert(iter->start.col < term->cols);

  struct coord abs_start = iter->start;
  abs_start.row = grid_row_absolute_in_view(grid, abs_start.row);

  struct coord abs_end = {term->cols - 1,
                          grid_row_absolute_in_view(grid, term->rows - 1)};

  /* BUG: matches *starting* outside the view, but ending *inside*, aren't
   * matched */
  struct range match;
  bool found = find_next(term, SEARCH_FORWARD, abs_start, abs_end, &match);
  if (!found)
    goto no_match;

  LOG_DBG("match at (absolute coordinates) %dx%d-%dx%d", match.start.row,
          match.start.col, match.end.row, match.end.col);

  /* Convert absolute row numbers back to view relative */
  match.start.row = match.start.row - grid->view + grid->num_rows;
  match.start.row &= grid->num_rows - 1;
  match.end.row = match.end.row - grid->view + grid->num_rows;
  match.end.row &= grid->num_rows - 1;

  LOG_DBG("match at (view-local coordinates) %dx%d-%dx%d, view=%d",
          match.start.row, match.start.col, match.end.row, match.end.col,
          grid->view);

  /* Assert match end comes *after* the match start */
  xassert(
      match.end.row > match.start.row ||
      (match.end.row == match.start.row && match.end.col >= match.start.col));

  /* Assert the match starts at, or after, the iterator position */
  xassert(match.start.row > iter->start.row ||
          (match.start.row == iter->start.row &&
           match.start.col >= iter->start.col));

  /* Continue at next column, next time */
  iter->start.row = match.start.row;
  iter->start.col = match.start.col + 1;

  if (iter->start.col >= term->cols) {
    iter->start.col = 0;
    iter->start.row++; /* Overflow is caught in next iteration */
  }

  xassert(iter->start.row >= 0);
  xassert(iter->start.row <= term->rows);
  xassert(iter->start.col >= 0);
  xassert(iter->start.col < term->cols);
  return match;

no_match:
  iter->start.row = -1;
  iter->start.col = -1;
  return (struct range){{-1, -1}, {-1, -1}};
}

static void add_wchars(struct terminal *term, char32_t *src, size_t count) {
  /* Strip non-printable characters */
  for (size_t i = 0, j = 0, orig_count = count; i < orig_count; i++) {
    if (isc32print(src[i]))
      src[j++] = src[i];
    else
      count--;
  }

  if (!search_ensure_size(term, term->vimode.search.len + count))
    return;

  xassert(term->vimode.search.len + count < term->vimode.search.sz);

  memmove(&term->vimode.search.buf[term->vimode.search.cursor + count],
          &term->vimode.search.buf[term->vimode.search.cursor],
          (term->vimode.search.len - term->vimode.search.cursor) *
              sizeof(char32_t));

  memcpy(&term->vimode.search.buf[term->vimode.search.cursor], src,
         count * sizeof(char32_t));

  term->vimode.search.len += count;
  term->vimode.search.cursor += count;
  term->vimode.search.buf[term->vimode.search.len] = U'\0';
}

void search_add_chars(struct terminal *term, const char *src, size_t count) {
  size_t chars = mbsntoc32(NULL, src, count, 0);
  if (chars == (size_t)-1) {
    LOG_ERRNO("failed to convert %.*s to Unicode", (int)count, src);
    return;
  }

  char32_t c32s[chars + 1];
  mbsntoc32(c32s, src, count, chars);
  add_wchars(term, c32s, chars);
}

// static size_t distance_next_word(const struct terminal *term) {
//   size_t cursor = term->vimode.search.cursor;
//
//   /* First eat non-whitespace. This is the word we're skipping past */
//   while (cursor < term->vimode.search.len) {
//     if (isc32space(term->vimode.search.buf[cursor++]))
//       break;
//   }
//
//   xassert(cursor == term->vimode.search.len ||
//           isc32space(term->vimode.search.buf[cursor - 1]));
//
//   /* Now skip past whitespace, so that we end up at the beginning of
//    * the next word */
//   while (cursor < term->vimode.search.len) {
//     if (!isc32space(term->vimode.search.buf[cursor++]))
//       break;
//   }
//
//   xassert(cursor == term->vimode.search.len ||
//           !isc32space(term->vimode.search.buf[cursor - 1]));
//
//   if (cursor < term->vimode.search.len &&
//   !isc32space(term->vimode.search.buf[cursor]))
//     cursor--;
//
//   return cursor - term->vimode.search.cursor;
// }

// static size_t distance_prev_word(const struct terminal *term) {
//   int cursor = term->vimode.search.cursor;
//
//   /* First, eat whitespace prefix */
//   while (cursor > 0) {
//     if (!isc32space(term->vimode.search.buf[--cursor]))
//       break;
//   }
//
//   xassert(cursor == 0 || !isc32space(term->vimode.search.buf[cursor]));
//
//   /* Now eat non-whitespace. This is the word we're skipping past */
//   while (cursor > 0) {
//     if (isc32space(term->vimode.search.buf[--cursor]))
//       break;
//   }
//
//   xassert(cursor == 0 || isc32space(term->vimode.search.buf[cursor]));
//   if (cursor > 0 && isc32space(term->vimode.search.buf[cursor]))
//     cursor++;
//
//   return term->vimode.search.cursor - cursor;
// }

void vimode_view_down(struct terminal *const term, int const delta) {
  if (!term->is_vimming) {
    return;
  }

  damage_cursor_cell(term);
  term->vimode.cursor.row -= delta;
  printf("VIMODE VIEW DOWN [delta=%d]\n", delta);
  clip_cursor_to_view(term);
}

// move_cursor_vertical
//
// Moves the cursor up or down within the scrollback. If the cursor goes outside
// the view bounds, the view is scrolled.
//
// Parameters:
// count - the number of rows to move by. Negative values move up, positive
//         down.
//
static void move_cursor_vertical(struct terminal *const term, int const count) {
  damage_cursor_cell(term);
  struct coord cursor = offset_to_view_relative(term, term->vimode.cursor);
  cursor.row += count;
  if (cursor.row < 0) {
    int const overflow = -cursor.row;
    cmd_scrollback_up(term, overflow);
    cursor.row = 0;
  } else if (cursor.row >= term->rows) {
    int const overflow = cursor.row - term->rows + 1;
    cmd_scrollback_down(term, overflow);
    cursor.row = term->rows - 1;
  }

  term->vimode.cursor = view_to_offset_relative(term, cursor);
  printf("DIRTYING CELL (%d, %d) [offset=%d; view=%d]\n",
         term->vimode.cursor.row, term->vimode.cursor.col, term->grid->offset,
         term->grid->view);
  damage_cursor_cell(term);
  render_refresh(term);
}

// move_cursor_horizontal
//
// Moves the cursor left or right within the scrollback. The cursor is
// clipped to the view bounds.
//
// Parameters:
// count - the number of columns to move by. Negative values move left, positive
//         right.
//
static void move_cursor_horizontal(struct terminal *const term,
                                   int const count) {
  damage_cursor_cell(term);
  struct coord cursor = term->vimode.cursor;
  cursor.col += count;
  if (cursor.col < 0) {
    cursor.col = 0;
  } else if (cursor.col >= term->cols) {
    cursor.col = term->cols - 1;
  }

  term->vimode.cursor = cursor;
  damage_cursor_cell(term);
  render_refresh(term);
}

static void execute_vimode_binding(struct seat *seat, struct terminal *term,
                                   const struct key_binding *binding,
                                   uint32_t serial) {
  const enum bind_action_vimode action = binding->action;

  if (term->grid != &term->normal) {
    return;
  }

  switch (action) {
  case BIND_ACTION_VIMODE_NONE:
    break;

  case BIND_ACTION_VIMODE_UP:
    move_cursor_vertical(term, -1);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_DOWN:
    move_cursor_vertical(term, 1);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_LEFT:
    move_cursor_horizontal(term, -1);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_RIGHT:
    move_cursor_horizontal(term, 1);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_UP_PAGE:
    cmd_scrollback_up(term, term->rows);
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_DOWN_PAGE:
    cmd_scrollback_down(term, term->rows);
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_UP_HALF_PAGE:
    cmd_scrollback_up(term, max(term->rows / 2, 1));
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_DOWN_HALF_PAGE:
    cmd_scrollback_down(term, max(term->rows / 2, 1));
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_UP_LINE:
    cmd_scrollback_up(term, 1);
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_DOWN_LINE:
    cmd_scrollback_down(term, 1);
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_FIRST_LINE:
    cmd_scrollback_up(term, term->grid->num_rows);
    term->vimode.cursor.row = -term->grid->num_rows;
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_LAST_LINE:
    cmd_scrollback_down(term, term->grid->num_rows);
    term->vimode.cursor.row = term->grid->num_rows;
    clip_cursor_to_view(term);
    update_selection(seat, term);
    break;

  case BIND_ACTION_VIMODE_CANCEL: {
    // We handle multiple actions here (in that exact order):
    // - clear search (handled by vimode-search bindings),
    // - return to the normal mode,
    // - exit vimode.
    if (is_mode_visual(term->vimode.mode)) {
      selection_cancel(term);
      term->vimode.mode = VI_MODE_NORMAL;
    } else {
      vimode_cancel(term);
    }
    break;
  }

  case BIND_ACTION_VIMODE_START_SEARCH:
    start_search(term);
    break;

  // TODO (kociap): Implement.
  case BIND_ACTION_VIMODE_FIND_NEXT:
  case BIND_ACTION_VIMODE_FIND_PREV:
    break;

  case BIND_ACTION_VIMODE_ENTER_VISUAL:
  case BIND_ACTION_VIMODE_ENTER_VLINE:
  case BIND_ACTION_VIMODE_ENTER_VBLOCK: {
    enum vi_mode mode = VI_MODE_VISUAL;
    if (action == BIND_ACTION_VIMODE_ENTER_VLINE) {
      mode = VI_MODE_VLINE;
    } else if (action == BIND_ACTION_VIMODE_ENTER_VBLOCK) {
      mode = VI_MODE_VBLOCK;
    }

    enum selection_kind const selection = selection_kind_from_vi_mode(mode);
    if (is_mode_visual(term->vimode.mode)) {
      // "Entering" the same mode exits it. Otherwise, we switch from
      // another visual mode.
      if (term->vimode.mode == mode) {
        selection_cancel(term);
        term->vimode.mode = VI_MODE_NORMAL;
      } else {
        selection_cancel(term);
        struct coord const start = term->vimode.selection.start;
        selection_start(term, start.col, start.row, selection, false);
        struct coord const cursor = term->grid->cursor.point;
        selection_update(term, cursor.col, cursor.row);
        term->vimode.mode = mode;
      }
    } else if (term->vimode.mode == VI_MODE_NORMAL) {
      struct coord const cursor = term->grid->cursor.point;
      selection_start(term, cursor.col, cursor.row, selection, false);
      selection_update(term, cursor.col, cursor.row);
      term->vimode.selection.start = cursor;
      term->vimode.mode = mode;
    }
    // render_refresh(term);
    break;
  }

  case BIND_ACTION_VIMODE_YANK:
    // TODO (kociap): Should yank executed in non-visual mode copy the
    // current line?
    if (is_mode_visual(term->vimode.mode)) {
      selection_finalize(seat, term, serial);
      // finalize only copies, but we also want to clear the selection
      selection_cancel(term);
      term->vimode.mode = VI_MODE_NORMAL;
    }
    break;

    // case BIND_ACTION_SEARCH_COMMIT:
    //   selection_finalize(seat, term, serial);
    //   search_cancel_keep_selection(term);
    //   return true;

    // case BIND_ACTION_SEARCH_CLIPBOARD_PASTE:
    //   text_from_clipboard(seat, term, &from_clipboard_cb,
    //   &from_clipboard_done,
    //                       term);
    //   *update_search_result = *redraw = true;
    //   return true;
    //
    // case BIND_ACTION_SEARCH_PRIMARY_PASTE:
    //   text_from_primary(seat, term, &from_clipboard_cb, &from_clipboard_done,
    //                     term);
    //   *update_search_result = *redraw = true;
    //   return true;
    //
    // case BIND_ACTION_SEARCH_UNICODE_INPUT:
    //   unicode_mode_activate(term);
    //   return true;

  case BIND_ACTION_VIMODE_COUNT:
    BUG("Invalid action type");
    break;

  default:
    BUG("Unhandled action type");
    break;
  }
}

static void execute_vimode_search_binding(struct seat *seat,
                                          struct terminal *term,
                                          const struct key_binding *binding,
                                          uint32_t serial) {
  const enum bind_action_vimode action = binding->action;
  struct vimode_search *const search = &term->vimode.search;

  if (term->grid != &term->normal) {
    return;
  }

  switch (action) {
  case BIND_ACTION_VIMODE_SEARCH_NONE:
    break;

  // TODO (kociap): implement
  case BIND_ACTION_VIMODE_SEARCH_CONFIRM:
    break;

  case BIND_ACTION_VIMODE_SEARCH_CANCEL:
    cancel_search(term);
    break;

  case BIND_ACTION_VIMODE_SEARCH_DELETE_PREV_CHAR:
    if (search->cursor > 0) {
      memmove(&search->buf[search->cursor - 1], &search->buf[search->cursor],
              (search->len - search->cursor) * sizeof(char32_t));
      search->cursor -= 1;
      search->len -= 1;
      search->buf[search->len] = U'\0';
      render_refresh_vimode_search_box(term);
    }
    break;

  case BIND_ACTION_VIMODE_SEARCH_LEFT:
    if (search->cursor > 0) {
      search->cursor -= 1;
      render_refresh_vimode_search_box(term);
    }
    break;

  case BIND_ACTION_VIMODE_SEARCH_RIGHT:
    if (search->cursor < search->len) {
      search->cursor += 1;
      render_refresh_vimode_search_box(term);
    }
    break;

  case BIND_ACTION_VIMODE_COUNT:
    BUG("Invalid action type");
    break;

  default:
    BUG("Unhandled action type");
    break;
  }
}

static struct key_binding const *
match_binding(key_binding_list_t const *bindings, uint32_t key,
              xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
              const xkb_keysym_t *raw_syms, size_t raw_count) {
  /* Match untranslated symbols */
  tll_foreach(*bindings, it) {
    const struct key_binding *bind = &it->item;

    if (bind->mods != mods || bind->mods == 0)
      continue;

    for (size_t i = 0; i < raw_count; i++) {
      if (bind->k.sym == raw_syms[i]) {
        return bind;
      }
    }
  }

  /* Match translated symbol */
  tll_foreach(*bindings, it) {
    const struct key_binding *bind = &it->item;

    if (bind->k.sym == sym && bind->mods == (mods & ~consumed)) {
      return bind;
    }
  }

  /* Match raw key code */
  tll_foreach(*bindings, it) {
    const struct key_binding *bind = &it->item;

    if (bind->mods != mods || bind->mods == 0)
      continue;

    tll_foreach(bind->k.key_codes, code) {
      if (code->item == key) {
        return bind;
      }
    }
  }

  return NULL;
}

void vimode_input(struct seat *seat, struct terminal *term,
                  const struct key_binding_set *bindings, uint32_t key,
                  xkb_keysym_t sym, xkb_mod_mask_t mods,
                  xkb_mod_mask_t consumed, const xkb_keysym_t *raw_syms,
                  size_t raw_count, uint32_t serial) {
  LOG_DBG("vimode: input: sym=%d/0x%x, mods=0x%08x, consumed=0x%08x", sym, sym,
          mods, consumed);

  enum xkb_compose_status compose_status =
      seat->kbd.xkb_compose_state != NULL
          ? xkb_compose_state_get_status(seat->kbd.xkb_compose_state)
          : XKB_COMPOSE_NOTHING;

  // bool update_search_result = false;

  if (!term->vimode.is_searching) {
    struct key_binding const *const binding = match_binding(
        &bindings->vimode, key, sym, mods, consumed, raw_syms, raw_count);
    if (binding != NULL) {
      execute_vimode_binding(seat, term, binding, serial);
    }
  } else {
    struct key_binding const *const binding =
        match_binding(&bindings->vimode_search, key, sym, mods, consumed,
                      raw_syms, raw_count);
    if (binding != NULL) {
      execute_vimode_search_binding(seat, term, binding, serial);
    } else {
      // If not a binding, then handle it as text input.
      uint8_t buf[64] = {0};
      int count = 0;

      if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(seat->kbd.xkb_compose_state,
                                           (char *)buf, sizeof(buf));
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);
      } else if (compose_status == XKB_COMPOSE_CANCELLED) {
        count = 0;
      } else {
        count = xkb_state_key_get_utf8(seat->kbd.xkb_state, key, (char *)buf,
                                       sizeof(buf));
      }

      // update_search_result = redraw = count > 0;

      if (count > 0) {
        search_add_chars(term, (const char *)buf, count);
        render_refresh_vimode_search_box(term);
      }
    }
  }

  LOG_DBG("search: buffer: %ls", (const wchar_t *)term->vimode.search.buf);
  // if (update_search_result)
  //   search_find_next(term, search_direction);
}
