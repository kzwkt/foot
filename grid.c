#include "grid.h"

//#include <string.h>
#include <assert.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 0
#include "log.h"

void
grid_swap_row(struct grid *grid, int row_a, int row_b)
{
    assert(grid->offset >= 0);
    assert(row_a != row_b);

    int real_a = (grid->offset + row_a + grid->num_rows) % grid->num_rows;
    int real_b = (grid->offset + row_b + grid->num_rows) % grid->num_rows;

    assert(real_a >= 0);
    assert(real_b >= 0);

    struct row *tmp = grid->rows[real_a];
    grid->rows[real_a] = grid->rows[real_b];
    grid->rows[real_b] = tmp;
}

struct row *
grid_row_alloc(int cols)
{
    struct row *row = malloc(sizeof(*row));
    row->cells = calloc(cols, sizeof(row->cells[0]));
    for (size_t c = 0; c < cols; c++)
        row->cells[c].attrs.clean = 1;
    row->dirty = false;  /* TODO: parameter? */
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
