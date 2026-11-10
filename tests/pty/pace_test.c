#include "pty/pty.h"

#include <assert.h>
#include <stdio.h>

/* The pace of the drain passes.
 *
 * A pass over PTY output ends on a budget. The budget grows while output keeps
 * streaming (pty_drain_budget_next), and the read chunk follows the parse time
 * of the last chunk (pty_chunk_next). A stream at speed must keep the largest
 * chunk, and a flood must keep the loop for longer than one frame.
 */

static void test_a_short_delay_keeps_the_chunk(void)
{
    /* 600 us of parse in a 2 ms pass: the chunk is not the cause, so it stays. */
    assert(pty_chunk_next(PTY_CHUNK_MAX, 600000ull, PTY_DRAIN_BUDGET_NS) ==
           PTY_CHUNK_MAX);
}

static void test_a_fast_stream_reads_the_maximum(void)
{
    /* 200 us of parse in a 10 ms pass doubles a small chunk. */
    size_t chunk = PTY_CHUNK_START;
    for (int i = 0; i < 8; i++)
        chunk = pty_chunk_next(chunk, 200000ull, PTY_DRAIN_BUDGET_MAX_NS);
    assert(chunk == PTY_CHUNK_MAX);
}

static void test_a_slow_stream_reads_less(void)
{
    /* 6 ms of parse in a 10 ms pass halves the chunk. */
    assert(pty_chunk_next(PTY_CHUNK_MAX, 6000000ull, PTY_DRAIN_BUDGET_MAX_NS) ==
           PTY_CHUNK_MAX / 2);
    /* The chunk never falls below its minimum. */
    assert(pty_chunk_next(PTY_CHUNK_MIN, 6000000ull, PTY_DRAIN_BUDGET_MAX_NS) ==
           PTY_CHUNK_MIN);
}

static void test_a_flood_grows_the_budget(void)
{
    uint64_t budget = PTY_DRAIN_BUDGET_NS;
    assert(pty_drain_budget_next(budget, true) == 4000000ull);
    budget = 4000000ull;
    assert(pty_drain_budget_next(budget, true) == 8000000ull);
    assert(pty_drain_budget_next(8000000ull, true) == PTY_DRAIN_BUDGET_MAX_NS);
    assert(pty_drain_budget_next(PTY_DRAIN_BUDGET_MAX_NS, true) ==
           PTY_DRAIN_BUDGET_MAX_NS);
    assert(PTY_DRAIN_BUDGET_MAX_NS > PTY_DRAIN_BUDGET_NS);
}

static void test_a_stream_that_runs_dry_returns_to_the_base_budget(void)
{
    assert(pty_drain_budget_next(PTY_DRAIN_BUDGET_MAX_NS, false) ==
           PTY_DRAIN_BUDGET_NS);
    /* Zero Is Initialization: an unused budget is the base budget. */
    assert(pty_drain_budget_next(0, true) == PTY_DRAIN_BUDGET_NS);
}

int main(void)
{
    test_a_short_delay_keeps_the_chunk();
    test_a_fast_stream_reads_the_maximum();
    test_a_slow_stream_reads_less();
    test_a_flood_grows_the_budget();
    test_a_stream_that_runs_dry_returns_to_the_base_budget();
    puts("PASS: a flood grows the drain budget and keeps the largest chunk");
    return 0;
}
