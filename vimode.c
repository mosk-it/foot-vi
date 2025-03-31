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

// TODO (kociap): consider adding scrolloff.
// TODO (kociap): consider not cancelling selection on scroll.
// TODO (kociap): jump list?

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

static enum search_direction invert_direction(enum search_direction direction) {
  return direction == SEARCH_FORWARD ? SEARCH_BACKWARD : SEARCH_FORWARD;
}

static struct coord cursor_to_view_relative(struct terminal *const term,
                                            struct coord cursor) {
  cursor.row += term->grid->offset;
  cursor.row -= term->grid->view;
  return cursor;
}

static struct coord cursor_from_view_relative(struct terminal *const term,
                                              struct coord coord) {
  coord.row += term->grid->view;
  coord.row -= term->grid->offset;
  return coord;
}

static int cursor_to_scrollback_relative(struct terminal *const term, int row) {
  row = grid_row_abs_to_sb(term->grid, term->rows,
                           grid_row_absolute(term->grid, row));
  return row;
}

static int cursor_from_scrollback_relative(struct terminal *const term,
                                           int row) {
  row = grid_row_sb_to_abs(term->grid, term->rows, row);
  row -= term->grid->offset;
  return row;
}

static int view_to_scrollback_relative(struct terminal *const term) {
  return grid_row_abs_to_sb(term->grid, term->rows, term->grid->view);
}

static struct coord delta_cursor_in_abs_coord(struct terminal *const term,
                                              struct coord const coord) {
  int const location = grid_row_abs_to_sb(term->grid, term->rows, coord.row);
  int const cursor =
      cursor_to_scrollback_relative(term, term->vimode.cursor.row);
  return (struct coord){
      .row = location - cursor,
      .col = coord.col - term->vimode.cursor.col,
  };
}

static void damage_cursor_cell(struct terminal *const term) {
  struct coord const cursor =
      cursor_to_view_relative(term, term->vimode.cursor);
  term_damage_cell_in_view(term, cursor.row, cursor.col);
  render_refresh(term);
}

static void clip_cursor_to_view(struct terminal *const term) {
  damage_cursor_cell(term);
  int cursor_row = cursor_to_scrollback_relative(term, term->vimode.cursor.row);
  int const view_row = view_to_scrollback_relative(term);
  if (cursor_row < view_row) {
    // Cursor is located above the current view. Move it to the top of
    // the view.
    cursor_row = view_row;
  } else if (cursor_row - view_row >= term->rows) {
    // Cursor is below the current view. Move it to the bottom of the
    // view.
    cursor_row = view_row + term->rows - 1;
  }
  term->vimode.cursor.row = cursor_from_scrollback_relative(term, cursor_row);
  LOG_DBG("CLIP CURSOR (%d, %d)\n", term->vimode.cursor.row,
          term->vimode.cursor.col);
  damage_cursor_cell(term);
  render_refresh(term);
}

static void center_view_on_cursor(struct terminal *const term) {
  int const cursor =
      cursor_to_scrollback_relative(term, term->vimode.cursor.row);
  int const current_view = view_to_scrollback_relative(term);
  int const half_viewport = term->rows / 2;
  int const delta = (cursor - half_viewport) - current_view;
  LOG_DBG("CENTER VIEW [cursor=(%d, %d); current_view=%d; half_viewport=%d; "
          "delta=%d]",
          cursor, term->vimode.cursor.col, current_view, half_viewport, delta);
  if (delta < 0) {
    cmd_scrollback_up(term, -delta);
  } else if (delta > 0) {
    cmd_scrollback_down(term, delta);
  }
}

static void update_selection(struct terminal *const term) {
  enum vi_mode const mode = term->vimode.mode;
  if (is_mode_visual(mode)) {
    struct coord const cursor =
        cursor_to_view_relative(term, term->vimode.cursor);
    selection_update(term, cursor);
    LOG_DBG(
        "UPDATE SELECTION [view=%d; cursor=(%d, %d); selection.end=(%d,%d)]",
        term->grid->view, cursor.row, cursor.col,
        term->selection.coords.end.row, term->selection.coords.end.col);
  }
}

static void damage_highlights(struct terminal *const term) {
  struct highlight_location const *location = term->vimode.highlights;
  int const offset = term->grid->offset;
  while (location != NULL) {
    struct coord const start = location->range.start;
    struct coord const end = location->range.end;
    for (int col = start.col; col <= end.col; col += 1) {
      term_damage_cell(term, start.row - offset, col);
    }
    location = location->next;
  }
  render_refresh(term);
}

static void clear_highlights(struct terminal *const term) {
  damage_highlights(term);
  struct highlight_location const *location = term->vimode.highlights;
  while (location != NULL) {
    struct highlight_location const *next = location->next;
    free((void *)location);
    location = next;
  }
  term->vimode.highlights = NULL;
}

// calculate_highlight_regions
//
// Build a list of regions (stored in term->vimode.highlights)
// consisting of all search matches within the current view. Uses the
// active search or the confirmed search if no active search.
//
// The regions are split so that each one spans at most
// a single line. The regions are in absolute row coordinates.
//
static void calculate_highlight_regions(struct terminal *const term) {
  char32_t const *search_buf = term->vimode.search.buf;
  size_t search_len = term->vimode.search.len;
  if (search_buf == NULL) {
    search_buf = term->vimode.confirmed_search.buf;
    search_len = term->vimode.confirmed_search.len;
  }

  struct highlight_location *start = NULL;
  struct highlight_location *current = NULL;
  struct search_match_iterator iter =
      search_matches_new_iter(term, search_buf, search_len);
  for (struct range match = search_matches_next(&iter); match.start.row >= 0;
       match = search_matches_next(&iter)) {
    int r = match.start.row;
    int start_col = match.start.col;
    int const end_row = match.end.row;

    while (true) {
      const int end_col = r == end_row ? match.end.col : term->cols - 1;
      struct highlight_location *location =
          xmalloc(sizeof(struct highlight_location));
      location->next = NULL;
      location->range = (struct range){
          .start.row = r,
          .start.col = start_col,
          .end.row = end_row,
          .end.col = end_col,
      };
      r += 1;
      start_col = 0;
      if (start != NULL) {
        current->next = location;
        current = location;
      } else {
        start = location;
        current = location;
      }
      if (r > end_row) {
        break;
      }
    }
  }
  term->vimode.highlights = start;
}

static void update_highlights(struct terminal *const term) {
  clear_highlights(term);
  calculate_highlight_regions(term);
  damage_highlights(term);
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

static void start_search(struct terminal *term,
                         enum search_direction const direction) {
  if (term->vimode.searching) {
    return;
  }

  LOG_DBG("VIMODE-SEARCH BEGIN");

  const struct grid *grid = term->grid;
  term->vimode.search.original_view = grid->view;
  term->vimode.search.original_cursor = term->vimode.cursor;
  term->vimode.search.len = 0;
  term->vimode.search.sz = 64;
  term->vimode.search.buf =
      xmalloc(term->vimode.search.sz * sizeof(term->vimode.search.buf[0]));
  term->vimode.search.buf[0] = U'\0';
  term->vimode.search.direction = direction;
  term->vimode.search.match = (struct coord){-1, -1};
  term->vimode.search.match_len = 0;
  term->vimode.searching = true;

  /* On-demand instantiate wayland surface */
  bool ret =
      wayl_win_subsurface_new(term->window, &term->window->search, false);
  xassert(ret);

  render_refresh_vimode_search_box(term);
}

static void restore_pre_search_state(struct terminal *const term) {
  damage_cursor_cell(term);
  term->vimode.cursor = term->vimode.search.original_cursor;
  damage_cursor_cell(term);
  term->grid->view =
      ensure_view_is_allocated(term, term->vimode.search.original_view);
  term_damage_view(term);
  render_refresh(term);
  update_selection(term);
}

static void cancel_search(struct terminal *const term,
                          bool const restore_original) {
  if (!term->vimode.searching) {
    return;
  }

  wayl_win_subsurface_destroy(&term->window->search);
  clear_highlights(term);
  term->vimode.searching = false;
  struct vimode_search *const search = &term->vimode.search;
  if (restore_original) {
    restore_pre_search_state(term);
  }

  free(search->buf);
  search->buf = NULL;
  search->len = search->sz = 0;
  search->cursor = 0;
  search->original_view = 0;
  search->match = (struct coord){-1, -1};
  search->match_len = 0;
  term->render.search_glyph_offset = 0;
}

static void confirm_search(struct terminal *const term) {
  if (term->vimode.confirmed_search.buf != NULL) {
    free(term->vimode.confirmed_search.buf);
  }
  term->vimode.confirmed_search.buf = term->vimode.search.buf;
  term->vimode.confirmed_search.len = term->vimode.search.len;
  term->vimode.confirmed_search.direction = term->vimode.search.direction;
  term->vimode.search.buf = NULL;
  cancel_search(term, false);
}

void vimode_search_begin(struct terminal *term) {
  vimode_begin(term);
  start_search(term, SEARCH_FORWARD);
  term_xcursor_update(term);
}

void vimode_begin(struct terminal *term) {
  LOG_DBG("VIMODE BEGIN");

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

  term->vimode.active = true;

  term_xcursor_update(term);
}

void vimode_cancel(struct terminal *term) {
  if (!term->vimode.active) {
    return;
  }

  LOG_DBG("VIMODE CANCEL");

  cancel_search(term, false);
  free(term->vimode.confirmed_search.buf);
  term->vimode.confirmed_search.buf = NULL;
  term->vimode.confirmed_search.len = 0;
  term->vimode.confirmed_search.direction = SEARCH_FORWARD;
  clear_highlights(term);
  selection_cancel(term);

  term->vimode.active = false;

  /* Reset IME state */
  if (term_ime_is_enabled(term)) {
    term_ime_disable(term);
    term_ime_enable(term);
  }

  struct grid *const grid = term->grid;
  grid->view = grid->offset;
  term_damage_view(term);
  term_xcursor_update(term);
  render_refresh(term);
}

static ssize_t matches_cell(const struct terminal *term,
                            const struct cell *cell, char32_t const *const buf,
                            size_t const len, size_t search_ofs) {
  assert(search_ofs < len);

  char32_t base = cell->wc;
  const struct composed *composed = NULL;

  if (base >= CELL_COMB_CHARS_LO && base <= CELL_COMB_CHARS_HI) {
    composed = composed_lookup(term->composed, base - CELL_COMB_CHARS_LO);
    base = composed->chars[0];
  }

  if (composed == NULL && base == 0 && buf[search_ofs] == U' ')
    return 1;

  if (c32ncasecmp(&base, buf, 1) != 0)
    return -1;

  if (composed != NULL) {
    if (search_ofs + composed->count > len)
      return -1;

    for (size_t j = 1; j < composed->count; j++) {
      if (composed->chars[j] != buf[search_ofs + j])
        return -1;
    }
  }

  return composed != NULL ? composed->count : 1;
}

static bool find_next(struct terminal *term, char32_t const *const buf,
                      size_t const len, enum search_direction direction,
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
      if (matches_cell(term, &row->cells[match_start_col], buf, len, 0) < 0) {
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

      for (size_t i = 0; i < len;) {
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
            matches_cell(term, &match_row->cells[match_end_col], buf, len, i);
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

      if (match_len != len) {
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
#undef ROW_DEC
}

static bool find_next_from_cursor(struct terminal *const term,
                                  char32_t *const buf, size_t const len,
                                  enum search_direction const direction,
                                  struct range *const match) {
  if (len == 0 || buf == NULL) {
    return false;
  }

  struct grid *grid = term->grid;

  struct coord start = {
      .row = grid_row_absolute(grid, term->vimode.cursor.row),
      .col = term->vimode.cursor.col,
  };

  // Step 1 character in the direction we are searching so that we do
  // not repeatedly match the same location.
  switch (direction) {
  case SEARCH_FORWARD:
    start.col += 1;
    if (start.col >= term->cols) {
      start.col = 0;
      start.row++;
      start.row &= grid->num_rows - 1;
    }
    break;

  case SEARCH_BACKWARD:
    start.col -= 1;
    if (start.col < 0) {
      start.col = term->cols - 1;
      start.row += grid->num_rows - 1;
      start.row &= grid->num_rows - 1;
    }
    break;
  }

  xassert(start.row >= 0 && start.col >= 0);

  struct coord end = start;
  switch (direction) {
  case SEARCH_FORWARD:
    /* Search forward, until we reach the cell *before* current start */
    end.col -= 1;
    if (end.col < 0) {
      end.col = term->cols - 1;
      end.row += grid->num_rows - 1;
      end.row &= grid->num_rows - 1;
    }
    break;

  case SEARCH_BACKWARD:
    /* Search backwards, until we reach the cell *after* current start */
    end.col += 1;
    if (end.col >= term->cols) {
      end.col = 0;
      end.row += 1;
      end.row &= grid->num_rows - 1;
    }
    break;
  }

  return find_next(term, buf, len, direction, start, end, match);
}

struct search_match_iterator
search_matches_new_iter(struct terminal *const term, char32_t const *const buf,
                        size_t const len) {
  return (struct search_match_iterator){
      .term = term,
      .start = {0, 0},
      .buf = buf,
      .len = len,
  };
}

struct range search_matches_next(struct search_match_iterator *iter) {
  struct terminal *term = iter->term;
  struct grid *grid = term->grid;
  if (iter->buf == NULL || iter->len == 0 || iter->start.row >= term->rows) {
    iter->start.row = -1;
    iter->start.col = -1;
    return (struct range){{-1, -1}, {-1, -1}};
  }

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
  bool found = find_next(term, iter->buf, iter->len, SEARCH_FORWARD, abs_start,
                         abs_end, &match);
  if (!found) {
    iter->start.row = -1;
    iter->start.col = -1;
    return (struct range){{-1, -1}, {-1, -1}};
  }

  LOG_DBG("match at (absolute coordinates) %dx%d-%dx%d", match.start.row,
          match.start.col, match.end.row, match.end.col);

  /* Assert match end comes *after* the match start */
  xassert(
      match.end.row > match.start.row ||
      (match.end.row == match.start.row && match.end.col >= match.start.col));

  /* Assert the match starts at, or after, the iterator position */
  xassert(
      match.start.row > abs_start.row ||
      (match.start.row == abs_start.row && match.start.col >= abs_start.col));

  /* Continue at next column, next time */
  iter->start.row += match.start.row - abs_start.row;
  iter->start.col = match.start.col + 1;

  if (iter->start.col >= term->cols) {
    iter->start.col = 0;
    iter->start.row += 1; /* Overflow is caught in next iteration */
  }

  xassert(iter->start.row >= 0);
  xassert(iter->start.row <= term->rows);
  xassert(iter->start.col >= 0);
  xassert(iter->start.col < term->cols);
  return match;
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

void vimode_view_down(struct terminal *const term, int const delta) {
  if (!term->vimode.active) {
    return;
  }

  LOG_DBG("VIMODE VIEW DOWN [delta=%d]", delta);
  damage_cursor_cell(term);
  term->vimode.cursor.row -= delta;
  clip_cursor_to_view(term);
  update_highlights(term);
}

// move_cursor_delta
//
// Moves the cursor within the scrollback by the vector delta. If the
// cursor goes outside the view bounds, the view is scrolled.
//
// Parameters:
// delta - the number of rows/columns to move by. Negative values move
//         up/left, positive down/right.
//
static void move_cursor_delta(struct terminal *const term,
                              struct coord const delta) {
  damage_cursor_cell(term);
  struct coord cursor = cursor_to_view_relative(term, term->vimode.cursor);
  cursor.row += delta.row;
  cursor.col += delta.col;

  if (cursor.row < 0) {
    int const overflow = -cursor.row;
    cmd_scrollback_up(term, overflow);
    cursor.row = 0;
  } else if (cursor.row >= term->rows) {
    int const overflow = cursor.row - term->rows + 1;
    cmd_scrollback_down(term, overflow);
    cursor.row = term->rows - 1;
  }

  if (cursor.col < 0) {
    cursor.col = 0;
  } else if (cursor.col >= term->cols) {
    cursor.col = term->cols - 1;
  }

  term->vimode.cursor = cursor_from_view_relative(term, cursor);
  LOG_DBG("CURSOR MOVED (%d, %d) [delta=(%d, %d)]", term->vimode.cursor.row,
          term->vimode.cursor.col, delta.row, delta.col);
  damage_cursor_cell(term);
  render_refresh(term);
}

static void move_cursor_vertical(struct terminal *const term, int const count) {
  move_cursor_delta(term, (struct coord){.row = count, .col = 0});
}

static void move_cursor_horizontal(struct terminal *const term,
                                   int const count) {
  move_cursor_delta(term, (struct coord){.row = 0, .col = count});
}

enum c32_class {
  CLASS_BLANK,
  CLASS_PUNCTUATION,
  CLASS_WORD,
};

enum c32_class get_class(char32_t const c) {
  if (c == '\0') {
    return CLASS_BLANK;
  }

  // We consider an underscore to be a word character instead of
  // punctuation.
  if (c == '_') {
    return CLASS_WORD;
  }

  bool const whitespace = isc32space(c);
  if (whitespace) {
    return CLASS_BLANK;
  }

  // TODO (kociap): unsure whether this handles all possible
  // punctuation, a subset of it or just the latin characters.
  bool const punctuation = isc32punct(c);
  if (punctuation) {
    return CLASS_PUNCTUATION;
  }

  // Most other characters may be considered "word" characters.
  return CLASS_WORD;
}

char32_t cursor_char(struct terminal *const term) {
  struct row *const row = grid_row(term->grid, term->vimode.cursor.row);
  return row->cells[term->vimode.cursor.col].wc;
}

enum c32_class cursor_class(struct terminal *const term) {
  char32_t const c = cursor_char(term);
  return get_class(c);
}

int row_length(struct terminal *const term, int const row_index) {
  struct row *const row = grid_row(term->grid, row_index);
  int length = 0;
  while (length < term->grid->num_cols) {
    if (row->cells[length].wc == '\0') {
      break;
    }
    length += 1;
  }
  return length;
}

// increment_cursor
//
// Move the cursor to the next character, moving to the next row as
// necessary.
//
// Returns:
// false when at the end of the scrollback.
// true otherwise.
//
bool increment_cursor(struct terminal *const term) {
  char32_t const c = cursor_char(term);
  // Within a row, move to the next column.
  if (c != '\0') {
    term->vimode.cursor.col += 1;
    struct row const *const row = grid_row(term->grid, term->vimode.cursor.row);
    // If the row does not contain a linebreak, we want to move to the
    // next row immediately.
    if (row->linebreak || term->vimode.cursor.col < term->cols) {
      return true;
    }
  }

  // Not in the last row.
  if (term->vimode.cursor.row != term->rows - 1) {
    term->vimode.cursor.row += 1;
    term->vimode.cursor.col = 0;
    // If the cursor moved outside the view, follow it.
    struct coord cursor = cursor_to_view_relative(term, term->vimode.cursor);
    if (cursor.row >= term->rows) {
      cmd_scrollback_down(term, 1);
    }
    return true;
  }

  return false;
}

// decrement_cursor
//
// Move the cursor to the previous character, moving to the previous
// row as necessary.
//
// Returns:
// false when at the start of the scrollback.
// true otherwise.
//
bool decrement_cursor(struct terminal *const term) {
  // Within a row, move to the previous column.
  if (term->vimode.cursor.col > 0) {
    term->vimode.cursor.col -= 1;
    return true;
  }

  // TODO (kociap): this seems like a terrible way (performance-wise)
  // to figure out the current scrollback position. Could maybe store
  // it in the grid instead of recalculating it repeatedly?
  int const sb_start =
      grid_sb_start_ignore_uninitialized(term->grid, term->rows);
  int const sb_row = grid_row_abs_to_sb_precalc_sb_start(
      term->grid, sb_start,
      grid_row_absolute(term->grid, term->vimode.cursor.row));
  // Not in the first row.
  if (sb_row > 0) {
    term->vimode.cursor.row -= 1;
    term->vimode.cursor.col = row_length(term, term->vimode.cursor.row) - 1;
    // If the cursor moved outside the view, follow it.
    struct coord cursor = cursor_to_view_relative(term, term->vimode.cursor);
    if (cursor.row < 0) {
      cmd_scrollback_up(term, 1);
    }
    return true;
  }
  return false;
}

// Skip characters of the same class.
//
// Returns:
// false when at the end of the scrollback.
// true otherwise.
//
bool skip_chars_forward(struct terminal *const term,
                        enum c32_class const class) {
  while (cursor_class(term) == class) {
    if (increment_cursor(term) == false) {
      return false;
    }
  }
  return true;
}

// Skip characters of the same class.
//
// Returns:
// false when at the end of the scrollback.
// true otherwise.
//
bool skip_chars_backward(struct terminal *const term,
                         enum c32_class const class) {
  while (cursor_class(term) == class) {
    if (decrement_cursor(term) == false) {
      return false;
    }
  }
  return true;
}

// MOTIONS

// Move the cursor back to the start of a word.
//
void motion_begin_word(struct terminal *const term) {
  if (decrement_cursor(term) == false) {
    return;
  }

  // Skip whitespace. If we encounter an empty row, stop.
  while (cursor_class(term) == CLASS_BLANK) {
    bool const row_empty = row_length(term, term->vimode.cursor.row) == 0;
    if (row_empty && term->vimode.cursor.col == 0) {
      return;
    }

    if (decrement_cursor(term) == false) {
      return;
    }
  }

  // Go to the start of the next word.
  enum c32_class const current_class = cursor_class(term);
  if (skip_chars_backward(term, current_class) == false) {
    return;
  }

  // We overshot. Move forward one character.
  increment_cursor(term);
}

// Move the cursor forward to the end of a word.
//
void motion_end_word(struct terminal *const term) {
  if (increment_cursor(term) == false) {
    return;
  }

  // Skip whitespace. If we encounter an empty row, stop.
  while (cursor_class(term) == CLASS_BLANK) {
    bool const row_empty = row_length(term, term->vimode.cursor.row) == 0;
    if (row_empty && term->vimode.cursor.col == 0) {
      return;
    }

    if (increment_cursor(term) == false) {
      return;
    }
  }

  // Go to the end of the next word.
  enum c32_class current_class = cursor_class(term);
  if (skip_chars_forward(term, current_class) == false) {
    return;
  }
  // We overshot. Go back one character.
  decrement_cursor(term);
}

// Move the cursor forward to the start of a word.
//
void motion_fwd_begin_word(struct terminal *const term) {
  enum c32_class const starting_class = cursor_class(term);
  if (increment_cursor(term) == false) {
    return;
  }

  // Move to the end of this word.
  if (starting_class != CLASS_BLANK) {
    if (skip_chars_forward(term, starting_class) == false) {

      return;
    }
  }

  // Skip whitespace. If we encounter an empty row, stop.
  while (cursor_class(term) == CLASS_BLANK) {
    bool const row_empty = row_length(term, term->vimode.cursor.row) == 0;
    if (row_empty && term->vimode.cursor.col == 0) {
      return;
    }

    if (increment_cursor(term) == false) {
      return;
    }
  }
}

// Move the cursor back to the end of a word.
//
void motion_back_end_word(struct terminal *const term) {
  enum c32_class const starting_class = cursor_class(term);
  if (decrement_cursor(term) == false) {
    return;
  }

  // Move to before the start of this word.
  if (starting_class != CLASS_BLANK) {
    if (skip_chars_backward(term, starting_class) == false) {
      return;
    }
  }

  // Skip whitespace. If we encounter an empty row, stop.
  while (cursor_class(term) == CLASS_BLANK) {
    bool const row_empty = row_length(term, term->vimode.cursor.row) == 0;
    if (row_empty && term->vimode.cursor.col == 0) {
      return;
    }

    if (decrement_cursor(term) == false) {
      return;
    }
  }
}

static void execute_vimode_binding(struct seat *seat, struct terminal *term,
                                   const struct key_binding *binding,
                                   uint32_t serial) {
  const enum bind_action_vimode action = binding->action;

  if (term->grid != &term->normal) {
    return;
  }
  LOG_DBG("PRE-ACTION DATA [offset=%d; view=%d]", term->grid->offset,
          term->grid->view);
  switch (action) {
  case BIND_ACTION_VIMODE_NONE:
    break;

  case BIND_ACTION_VIMODE_UP:
    move_cursor_vertical(term, -1);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_DOWN:
    move_cursor_vertical(term, 1);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_LEFT:
    move_cursor_horizontal(term, -1);
    update_selection(term);
    break;

  case BIND_ACTION_VIMODE_RIGHT:
    move_cursor_horizontal(term, 1);
    update_selection(term);
    break;

  case BIND_ACTION_VIMODE_UP_PAGE:
    cmd_scrollback_up(term, term->rows);
    clip_cursor_to_view(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_DOWN_PAGE:
    cmd_scrollback_down(term, term->rows);
    clip_cursor_to_view(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_UP_HALF_PAGE:
    cmd_scrollback_up(term, max(term->rows / 2, 1));
    clip_cursor_to_view(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_DOWN_HALF_PAGE:
    cmd_scrollback_down(term, max(term->rows / 2, 1));
    clip_cursor_to_view(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_UP_LINE:
    cmd_scrollback_up(term, 1);
    clip_cursor_to_view(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_DOWN_LINE:
    cmd_scrollback_down(term, 1);
    clip_cursor_to_view(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_FIRST_LINE: {
    cmd_scrollback_up(term, term->grid->num_rows);
    damage_cursor_cell(term);
    int const view_row = view_to_scrollback_relative(term);
    term->vimode.cursor.row = cursor_from_scrollback_relative(term, view_row);
    damage_cursor_cell(term);
    update_selection(term);
    update_highlights(term);
  } break;

  case BIND_ACTION_VIMODE_LAST_LINE:
    cmd_scrollback_down(term, term->grid->num_rows);
    damage_cursor_cell(term);
    term->vimode.cursor.row = term->rows - 1;
    damage_cursor_cell(term);
    update_selection(term);
    update_highlights(term);
    break;

  case BIND_ACTION_VIMODE_LINE_BEGIN:
    damage_cursor_cell(term);
    term->vimode.cursor.col = 0;
    damage_cursor_cell(term);
    break;

  case BIND_ACTION_VIMODE_LINE_END: {
    damage_cursor_cell(term);
    struct row const *const row = grid_row(term->grid, term->vimode.cursor.row);
    int col = term->cols - 1;
    while (col > 0) {
      if (row->cells[col].wc != '\0') {
        break;
      }
      col -= 1;
    }
    term->vimode.cursor.col = col;
    damage_cursor_cell(term);
  } break;

  case BIND_ACTION_VIMODE_TEXT_BEGIN: {
    damage_cursor_cell(term);
    struct row const *const row = grid_row(term->grid, term->vimode.cursor.row);
    int col = 0;
    while (col < term->cols - 1) {
      if (isc32graph(row->cells[col].wc)) {
        break;
      }
      col += 1;
    }
    term->vimode.cursor.col = col;
    damage_cursor_cell(term);
  } break;

  case BIND_ACTION_VIMODE_WORD_BEGIN:
    damage_cursor_cell(term);
    motion_begin_word(term);
    damage_cursor_cell(term);
    update_selection(term);
    break;

  case BIND_ACTION_VIMODE_WORD_END:
    damage_cursor_cell(term);
    motion_end_word(term);
    damage_cursor_cell(term);
    update_selection(term);
    break;

  case BIND_ACTION_VIMODE_NEXT_WORD_BEGIN:
    damage_cursor_cell(term);
    motion_fwd_begin_word(term);
    damage_cursor_cell(term);
    update_selection(term);
    break;

  case BIND_ACTION_VIMODE_PREV_WORD_END:
    damage_cursor_cell(term);
    motion_back_end_word(term);
    damage_cursor_cell(term);
    update_selection(term);
    break;

  case BIND_ACTION_VIMODE_CANCEL: {
    // We handle multiple actions here (in that exact order):
    // - return to the normal mode,
    // - exit vimode.
    // Clearing search is handled by vimode-search bindings.
    if (is_mode_visual(term->vimode.mode)) {
      selection_cancel(term);
      term->vimode.mode = VI_MODE_NORMAL;
    } else {
      vimode_cancel(term);
    }
  } break;

  case BIND_ACTION_VIMODE_START_SEARCH_FORWARD:
    start_search(term, SEARCH_FORWARD);
    break;

  case BIND_ACTION_VIMODE_START_SEARCH_BACKWARD:
    start_search(term, SEARCH_BACKWARD);
    break;

  case BIND_ACTION_VIMODE_FIND_PREV:
  case BIND_ACTION_VIMODE_FIND_NEXT: {
    enum search_direction const direction =
        (action == BIND_ACTION_VIMODE_FIND_NEXT)
            ? term->vimode.confirmed_search.direction
            : invert_direction(term->vimode.confirmed_search.direction);
    struct range match;
    bool const matched = find_next_from_cursor(
        term, term->vimode.confirmed_search.buf,
        term->vimode.confirmed_search.len, direction, &match);
    // TODO (kociap): feedback for the user when no match?
    if (matched) {
      struct coord const delta = delta_cursor_in_abs_coord(term, match.start);
      LOG_DBG("FIND %s [direction=%s; location=%d; cursor=%d; match=(%d, "
              "%d)]\n",
              (action == BIND_ACTION_VIMODE_FIND_NEXT) ? "NEXT" : "PREV",
              (direction == SEARCH_FORWARD ? "forward" : "backward"),
              grid_row_abs_to_sb(term->grid, term->rows, match.start.row),
              cursor_to_scrollback_relative(term, term->vimode.cursor.row),
              match.start.row, match.start.col);
      move_cursor_delta(term, delta);
      update_selection(term);
    }
    update_highlights(term);
  } break;

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
        selection_start(term, start, selection, false);
        struct coord const cursor =
            cursor_to_view_relative(term, term->vimode.cursor);
        selection_update(term, cursor);
        term->vimode.mode = mode;
      }
    } else if (term->vimode.mode == VI_MODE_NORMAL) {
      // Enter the visual mode.
      struct coord const cursor =
          cursor_to_view_relative(term, term->vimode.cursor);
      selection_start(term, cursor, selection, false);
      term->vimode.selection.start = cursor;
      term->vimode.mode = mode;
    }
  } break;

  case BIND_ACTION_VIMODE_YANK:
    // TODO (kociap): Should yank executed in non-visual mode copy the
    // current line?
    if (is_mode_visual(term->vimode.mode)) {
      // Copy, clear the selection and exit the visual mode.
      selection_finalize(seat, term, serial);
      selection_cancel(term);
      term->vimode.mode = VI_MODE_NORMAL;
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

static void execute_vimode_search_binding(struct seat *seat,
                                          struct terminal *const term,
                                          const struct key_binding *binding,
                                          uint32_t serial,
                                          bool *search_string_changed) {
  const enum bind_action_vimode action = binding->action;
  struct vimode_search *const search = &term->vimode.search;
  *search_string_changed = false;

  if (term->grid != &term->normal) {
    return;
  }

  switch (action) {
  case BIND_ACTION_VIMODE_SEARCH_NONE:
    break;

  case BIND_ACTION_VIMODE_SEARCH_CONFIRM:
    if (search->match_len > 0) {
      struct coord const delta = delta_cursor_in_abs_coord(term, search->match);
      LOG_DBG("CONFIRM SEARCH [location=%d; cursor=%d; match=(%d, %d)]",
              grid_row_abs_to_sb(term->grid, term->rows, search->match.row),
              cursor_to_scrollback_relative(term, term->vimode.cursor.row),
              search->match.row, search->match.col);
      move_cursor_delta(term, delta);
      center_view_on_cursor(term);
      update_selection(term);
    }
    confirm_search(term);
    break;

  case BIND_ACTION_VIMODE_SEARCH_CANCEL:
    cancel_search(term, true);
    break;

  case BIND_ACTION_VIMODE_SEARCH_DELETE_PREV_CHAR:
    if (search->cursor > 0) {
      memmove(&search->buf[search->cursor - 1], &search->buf[search->cursor],
              (search->len - search->cursor) * sizeof(char32_t));
      search->cursor -= 1;
      search->len -= 1;
      search->buf[search->len] = U'\0';
      render_refresh_vimode_search_box(term);
      *search_string_changed = true;
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
  LOG_DBG("VIMODE INPUT [sym=%d/0x%x, mods=0x%08x, consumed=0x%08x]", sym, sym,
          mods, consumed);

  enum xkb_compose_status compose_status =
      seat->kbd.xkb_compose_state != NULL
          ? xkb_compose_state_get_status(seat->kbd.xkb_compose_state)
          : XKB_COMPOSE_NOTHING;

  if (!term->vimode.searching) {
    struct key_binding const *const binding = match_binding(
        &bindings->vimode, key, sym, mods, consumed, raw_syms, raw_count);
    if (binding != NULL) {
      execute_vimode_binding(seat, term, binding, serial);
    }
  } else {
    struct key_binding const *const binding =
        match_binding(&bindings->vimode_search, key, sym, mods, consumed,
                      raw_syms, raw_count);
    bool search_string_updated = false;
    if (binding != NULL) {
      execute_vimode_search_binding(seat, term, binding, serial,
                                    &search_string_updated);
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

      if (count > 0) {
        search_add_chars(term, (const char *)buf, count);
        render_refresh_vimode_search_box(term);
        search_string_updated = true;
      }
    }

    if (search_string_updated) {
      LOG_DBG("SEARCH UPDATED [%ls]", (const wchar_t *)term->vimode.search.buf);
      struct range match;
      // TODO (kociap): when several consecutive searches succeed, the
      // cursor is not moved to its original position in-between
      // searches.
      bool const matched = find_next_from_cursor(
          term, term->vimode.search.buf, term->vimode.search.len,
          term->vimode.search.direction, &match);
      if (matched > 0) {
        term->vimode.search.match = match.start;
        term->vimode.search.match_len = term->vimode.search.len;
        struct coord const delta =
            delta_cursor_in_abs_coord(term, term->vimode.search.match);
        move_cursor_delta(term, delta);
        center_view_on_cursor(term);
        update_selection(term);
      } else {
        restore_pre_search_state(term);
      }
      update_highlights(term);
    }
  }
}
