#include "grid.h"

#include <string.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "sixel.h"
#include "util.h"
#include "xmalloc.h"

void
grid_swap_row(struct grid *grid, int row_a, int row_b)
{
    xassert(grid->offset >= 0);
    xassert(row_a != row_b);

    int real_a = (grid->offset + row_a) & (grid->num_rows - 1);
    int real_b = (grid->offset + row_b) & (grid->num_rows - 1);

    struct row *a = grid->rows[real_a];
    struct row *b = grid->rows[real_b];

    grid->rows[real_a] = b;
    grid->rows[real_b] = a;
}

struct row *
grid_row_alloc(int cols, bool initialize)
{
    struct row *row = xmalloc(sizeof(*row));
    row->dirty = false;
    row->linebreak = false;

    if (initialize) {
        row->cells = xcalloc(cols, sizeof(row->cells[0]));
        for (size_t c = 0; c < cols; c++)
            row->cells[c].attrs.clean = 1;
    } else
        row->cells = xmalloc(cols * sizeof(row->cells[0]));

    return row;
}

void
grid_row_free(struct row *row)
{
    if (row == NULL)
        return;

    free(row->cells);
    free(row);
}

void
grid_resize_without_reflow(
    struct grid *grid, int new_rows, int new_cols,
    int old_screen_rows, int new_screen_rows)
{
    struct row *const *old_grid = grid->rows;
    const int old_rows = grid->num_rows;
    const int old_cols = grid->num_cols;

    struct row **new_grid = xcalloc(new_rows, sizeof(new_grid[0]));

    tll(struct sixel) untranslated_sixels = tll_init();
    tll_foreach(grid->sixel_images, it)
        tll_push_back(untranslated_sixels, it->item);
    tll_free(grid->sixel_images);

    int new_offset = 0;

    /* Copy old lines, truncating them if old rows were longer */
    for (int r = 0, n = min(old_screen_rows, new_screen_rows); r < n; r++) {
        const int old_row_idx = (grid->offset + r) & (old_rows - 1);
        const int new_row_idx = (new_offset + r) & (new_rows - 1);

        const struct row *old_row = old_grid[old_row_idx];
        xassert(old_row != NULL);

        struct row *new_row = grid_row_alloc(new_cols, false);
        new_grid[new_row_idx] = new_row;

        memcpy(new_row->cells,
               old_row->cells,
               sizeof(struct cell) * min(old_cols, new_cols));

        new_row->dirty = old_row->dirty;
        new_row->linebreak = false;

        /* Clear "new" columns */
        if (new_cols > old_cols) {
            memset(&new_row->cells[old_cols], 0,
                   sizeof(struct cell) * (new_cols - old_cols));
            new_row->dirty = true;
        }

        /* Map sixels on current "old" row to current "new row" */
        tll_foreach(untranslated_sixels, it) {
            if (it->item.pos.row != old_row_idx)
                continue;

            struct sixel sixel = it->item;
            sixel.pos.row = new_row_idx;

            if (sixel.pos.col < new_cols)
                tll_push_back(grid->sixel_images, sixel);
            else
                sixel_destroy(&it->item);
            tll_remove(untranslated_sixels, it);
        }
    }

    /* Clear "new" lines */
    for (int r = min(old_screen_rows, new_screen_rows); r < new_screen_rows; r++) {
        struct row *new_row = grid_row_alloc(new_cols, false);
        new_grid[(new_offset + r) & (new_rows - 1)] = new_row;

        memset(new_row->cells, 0, sizeof(struct cell) * new_cols);
        new_row->linebreak = false;
        new_row->dirty = true;
    }

    /* Free old grid */
    for (int r = 0; r < grid->num_rows; r++)
        grid_row_free(old_grid[r]);
    free(grid->rows);

    grid->rows = new_grid;
    grid->num_rows = new_rows;
    grid->num_cols = new_cols;

    grid->view = grid->offset = new_offset;

    /* Keep cursor at current position, but clamp to new dimensions */
    struct coord cursor = grid->cursor.point;
    if (cursor.row == old_screen_rows - 1) {
        /* 'less' breaks if the cursor isn't at the bottom */
        cursor.row = new_screen_rows - 1;
    }
    cursor.row = min(cursor.row, new_screen_rows - 1);
    cursor.col = min(cursor.col, new_cols - 1);
    grid->cursor.point = cursor;

    struct coord saved_cursor = grid->saved_cursor.point;
    if (saved_cursor.row == old_screen_rows - 1)
        saved_cursor.row = new_screen_rows - 1;
    saved_cursor.row = min(saved_cursor.row, new_screen_rows - 1);
    saved_cursor.col = min(saved_cursor.col, new_cols - 1);
    grid->saved_cursor.point = saved_cursor;

    grid->cur_row = new_grid[(grid->offset + cursor.row) & (new_rows - 1)];
    grid->cursor.lcf = false;
    grid->saved_cursor.lcf = false;

    /* Free sixels we failed to "map" to the new grid */
    tll_foreach(untranslated_sixels, it)
        sixel_destroy(&it->item);
    tll_free(untranslated_sixels);

#if defined(_DEBUG)
    for (int r = 0; r < new_screen_rows; r++)
        grid_row_in_view(grid, r);
#endif
}

void
grid_resize_and_reflow(
    struct grid *grid, int new_rows, int new_cols,
    int old_screen_rows, int new_screen_rows,
    size_t tracking_points_count,
    struct coord *const _tracking_points[static tracking_points_count],
    size_t compose_count, const struct
    composed composed[static compose_count])
{
    struct row *const *old_grid = grid->rows;
    const int old_rows = grid->num_rows;
    const int old_cols = grid->num_cols;

    /* Is viewpoint tracking current grid offset? */
    const bool view_follows = grid->view == grid->offset;

    int new_col_idx = 0;
    int new_row_idx = 0;

    struct row **new_grid = xcalloc(new_rows, sizeof(new_grid[0]));
    struct row *new_row = new_grid[new_row_idx];

    xassert(new_row == NULL);
    new_row = grid_row_alloc(new_cols, true);
    new_grid[new_row_idx] = new_row;

    /* Start at the beginning of the old grid's scrollback. That is,
     * at the output that is *oldest* */
    int offset = grid->offset + old_screen_rows;

    tll(struct sixel) untranslated_sixels = tll_init();
    tll_foreach(grid->sixel_images, it)
        tll_push_back(untranslated_sixels, it->item);
    tll_free(grid->sixel_images);

    /* Turn cursor coordinates into grid absolute coordinates */
    struct coord cursor = grid->cursor.point;
    cursor.row += grid->offset;
    cursor.row &= old_rows - 1;

    struct coord saved_cursor = grid->saved_cursor.point;
    saved_cursor.row += grid->offset;
    saved_cursor.row &= old_rows - 1;

    tll(struct coord *) tracking_points = tll_init();
    tll_push_back(tracking_points, &cursor);
    tll_push_back(tracking_points, &saved_cursor);

    struct coord viewport = {0, grid->view};
    if (!view_follows)
        tll_push_back(tracking_points, &viewport);

    for (size_t i = 0; i < tracking_points_count; i++)
        tll_push_back(tracking_points, _tracking_points[i]);

    /*
     * Walk the old grid
     */
    for (int r = 0; r < old_rows; r++) {

        const size_t old_row_idx = (offset + r) & (old_rows - 1);

        /* Unallocated (empty) rows we can simply skip */
        const struct row *old_row = old_grid[old_row_idx];
        if (old_row == NULL)
            continue;

        /* Map sixels on current "old" row to current "new row" */
        tll_foreach(untranslated_sixels, it) {
            if (it->item.pos.row != old_row_idx)
                continue;

            struct sixel sixel = it->item;
            sixel.pos.row = new_row_idx;

            tll_push_back(grid->sixel_images, sixel);
            tll_remove(untranslated_sixels, it);
        }

#define line_wrap()                                                     \
        do {                                                            \
            new_col_idx = 0;                                            \
            new_row_idx = (new_row_idx + 1) & (new_rows - 1);           \
                                                                        \
            new_row = new_grid[new_row_idx];                            \
            if (new_row == NULL) {                                      \
                new_row = grid_row_alloc(new_cols, true);               \
                new_grid[new_row_idx] = new_row;                        \
            } else {                                                    \
                memset(new_row->cells, 0, new_cols * sizeof(new_row->cells[0])); \
                new_row->linebreak = false;                             \
                tll_foreach(grid->sixel_images, it) {                   \
                    if (it->item.pos.row == new_row_idx) {              \
                        sixel_destroy(&it->item);                       \
                        tll_remove(grid->sixel_images, it);             \
                    }                                                   \
                }                                                       \
            }                                                           \
        } while(0)

#define print_spacer() \
        do { \
            new_row->cells[new_col_idx].wc = CELL_MULT_COL_SPACER;      \
            new_row->cells[new_col_idx].attrs = old_cell->attrs;        \
            new_row->cells[new_col_idx].attrs.clean = 1;                \
        } while (0)

        /*
         * Keep track of empty cells. If the old line ends with a
         * string of empty cells, we don't need to, nor do we want to,
         * add those to the new line. However, if there are non-empty
         * cells *after* the string of empty cells, we need to emit
         * the empty cells too. And that may trigger linebreaks
         */
        int empty_count = 0;

        /* Walk current line of the old grid */
        for (int c = 0; c < old_cols; c++) {

            /* Check if this cell is one of the tracked cells */
            bool is_tracking_point = false;
            tll_foreach(tracking_points, it) {
                if (it->item->row == old_row_idx && it->item->col == c) {
                    is_tracking_point = true;
                    break;
                }
            }

            if (old_row->cells[c].wc == 0 && !is_tracking_point) {
                empty_count++;
                continue;
            }

            /* Allow left-adjusted and right-adjusted text, with empty
             * cells in between, to be "pushed together" */
            int old_cols_left = old_cols - c;
            int cols_needed = empty_count + old_cols_left;
            int new_cols_left = new_cols - new_col_idx;
            if (new_cols_left < cols_needed && new_cols_left >= old_cols_left)
                empty_count = max(0, empty_count - (cols_needed - new_cols_left));

            wchar_t wc = old_row->cells[c].wc;
            if (wc >= CELL_COMB_CHARS_LO &&
                wc < (CELL_COMB_CHARS_LO + compose_count))
            {
                wc = composed[wc - CELL_COMB_CHARS_LO].base;
            }

            int width = max(1, wcwidth(wc));

            /* Multi-column characters are never cut in half */
            xassert(c + width <= old_cols);

            for (int i = 0; i < empty_count + 1; i++) {
                const struct cell *old_cell = &old_row->cells[c - empty_count + i];
                wc = old_cell->wc;

                if (wc == CELL_MULT_COL_SPACER)
                    continue;

                if (wc >= CELL_COMB_CHARS_LO &&
                    wc < (CELL_COMB_CHARS_LO + compose_count))
                {
                    wc = composed[wc - CELL_COMB_CHARS_LO].base;
                }

                /* Out of columns on current row in new grid? */
                if (new_col_idx + max(1, wcwidth(wc)) > new_cols) {
                    /* Pad to end-of-line with spacers, then line-wrap */
                    for (;new_col_idx < new_cols; new_col_idx++)
                        print_spacer();
                    line_wrap();
                }

                xassert(new_row != NULL);
                xassert(new_col_idx >= 0);
                xassert(new_col_idx < new_cols);

                new_row->cells[new_col_idx] = *old_cell;
                new_row->cells[new_col_idx].attrs.clean = 1;

                /* Translate tracking point(s) */
                if (is_tracking_point && i >= empty_count) {
                    tll_foreach(tracking_points, it) {
                        if (it->item->row == old_row_idx && it->item->col == c) {
                            it->item->row = new_row_idx;
                            it->item->col = new_col_idx;
                            tll_remove(tracking_points, it);
                        }
                    }
                }
                new_col_idx++;
            }

            /* For multi-column characters, insert spacers in the
             * subsequent cells */
            const struct cell *old_cell = &old_row->cells[c];
            for (size_t i = 0; i < width - 1; i++) {
                xassert(new_col_idx < new_cols);
                print_spacer();
                new_col_idx++;
            }

            c += width - 1;
            empty_count = 0;
        }

        if (old_row->linebreak) {
            new_row->linebreak = true;
            line_wrap();
        }

#undef print_spacer
#undef line_wrap
    }

    /* Set offset such that the last reflowed row is at the bottom */
    grid->offset = new_row_idx - new_screen_rows + 1;
    while (grid->offset < 0)
        grid->offset += new_rows;
    while (new_grid[grid->offset] == NULL)
        grid->offset = (grid->offset + 1) & (new_rows - 1);

    /* Ensure all visible rows have been allocated */
    for (int r = 0; r < new_screen_rows; r++) {
        int idx = (grid->offset + r) & (new_rows - 1);
        if (new_grid[idx] == NULL)
            new_grid[idx] = grid_row_alloc(new_cols, true);
    }

    grid->view = view_follows ? grid->offset : viewport.row;

    /* If enlarging the window, the old viewport may be too far down,
     * with unallocated rows. Make sure this cannot happen */
    while (true) {
        int idx = (grid->view + new_screen_rows - 1) & (new_rows - 1);
        if (new_grid[idx] != NULL)
            break;
        grid->view--;
        if (grid->view < 0)
            grid->view += new_rows;
    }
    for (size_t r = 0; r < new_screen_rows; r++) {
        int UNUSED idx = (grid->view + r) & (new_rows - 1);
        xassert(new_grid[idx] != NULL);
    }

    /* Free old grid */
    for (int r = 0; r < grid->num_rows; r++)
        grid_row_free(old_grid[r]);
    free(grid->rows);

    grid->rows = new_grid;
    grid->num_rows = new_rows;
    grid->num_cols = new_cols;

    /* Convert absolute coordinates to screen relative */
    cursor.row -= grid->offset;
    while (cursor.row < 0)
        cursor.row += grid->num_rows;
    cursor.row = min(cursor.row, new_screen_rows - 1);
    cursor.col = min(cursor.col, new_cols - 1);

    saved_cursor.row -= grid->offset;
    while (saved_cursor.row < 0)
        saved_cursor.row += grid->num_rows;
    saved_cursor.row = min(saved_cursor.row, new_screen_rows - 1);
    saved_cursor.col = min(saved_cursor.col, new_cols - 1);

    grid->cur_row = new_grid[(grid->offset + cursor.row) & (new_rows - 1)];
    grid->cursor.point = cursor;
    grid->saved_cursor.point = saved_cursor;

    grid->cursor.lcf = false;
    grid->saved_cursor.lcf = false;

    /* Free sixels we failed to "map" to the new grid */
    tll_foreach(untranslated_sixels, it)
        sixel_destroy(&it->item);
    tll_free(untranslated_sixels);

    tll_free(tracking_points);
}
