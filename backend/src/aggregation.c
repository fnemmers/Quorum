#include "aggregation.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * State of each bucket in the hash table.
 */
enum {
    ENTRY_EMPTY = 0,
    ENTRY_OCCUPIED = 1
};

/*
 * One bucket in the hash table.
 */
typedef struct {
    char symbol[MAX_SYMBOL_LEN];
    int count;
    uint8_t state;
} Entry;

/*
 * The Aggregator struct is the full hash map.
 */
struct Aggregator {
    Entry *buckets;
    size_t cap;
    size_t n_distinct;
    size_t total_picks;
};

/*
 * FNV-1a hash:
 * turns a ticker string into a 64-bit integer.
 *
 * Example:
 *   "AAPL" -> some large numeric hash value
 *
 * We use that hash value to choose the starting bucket.
 */
static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;

    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }

    return h;
}

/*
 * Round a number up to the next power of two.
 *
 * Why:
 *   if capacity is a power of two, bucket lookup can use:
 *       hash & (cap - 1)
 *   which is faster than modulo.
 *
 * Example:
 *   1500 -> 2048
 */
static size_t next_power_of_two(size_t x) {
    size_t p = 1;

    while (p < x) {
        p <<= 1;
    }

    return p;
}

/*
 * Convert a hash into a valid bucket index.
 *
 * Because cap is a power of two, this gives us
 * a number between 0 and cap - 1.
 */
static size_t bucket_index(uint64_t h, size_t cap) {
    return h & (cap - 1);
}

/*
 * Resize the table and reinsert all occupied entries.
 *
 * This matters if the load factor gets too high.
 * More full table -> more collisions -> slower probing.
 *
 * Steps:
 *   1. save old buckets
 *   2. allocate a larger empty bucket array
 *   3. walk old buckets
 *   4. re-hash every occupied entry into the new array
 *   5. free old array
 */
static int agg_rehash(Aggregator *a, size_t new_cap) {
    Entry *old_buckets = a->buckets;
    size_t old_cap = a->cap;

    Entry *new_buckets = calloc(new_cap, sizeof(Entry));
    if (new_buckets == NULL) {
        return -1;
    }

    a->buckets = new_buckets;
    a->cap = new_cap;
    a->n_distinct = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_buckets[i].state != ENTRY_OCCUPIED) {
            continue;
        }

        uint64_t h = fnv1a(old_buckets[i].symbol);
        size_t idx = bucket_index(h, a->cap);

        /*
         * Linear probing:
         * if the desired slot is full, move forward until
         * an empty one is found.
         */
        while (a->buckets[idx].state == ENTRY_OCCUPIED) {
            idx = (idx + 1) & (a->cap - 1);
        }

        /*
         * Copy the old entry into the new table.
         * This preserves the ticker and its count.
         */
        a->buckets[idx] = old_buckets[i];
        a->n_distinct++;
    }

    free(old_buckets);
    return 0;
}

/*
 * Create a new aggregator object.
 */
Aggregator *agg_create(size_t initial_capacity) {
    Aggregator *a;
    size_t cap;

    /*
     * If caller gives 0, use a practical default.
     * 2048 is more than enough for ~500 distinct S&P tickers.
     */
    if (initial_capacity == 0) {
        initial_capacity = 2048;
    }

    /*
     * Force capacity to be a power of two.
     */
    cap = next_power_of_two(initial_capacity);

    /*
     * Avoid absurdly tiny tables.
     */
    if (cap < 8) {
        cap = 8;
    }

    /*
     * Allocate the main Aggregator object.
     */
    a = malloc(sizeof(*a));
    if (a == NULL) {
        return NULL;
    }

    /*
     * Allocate and zero the bucket array.
     */
    a->buckets = calloc(cap, sizeof(Entry));
    if (a->buckets == NULL) {
        free(a);
        return NULL;
    }

    /*
     * Initialize metadata fields.
     */
    a->cap = cap;
    a->n_distinct = 0;
    a->total_picks = 0;

    return a;
}

/*
 * Free all memory owned by the aggregator.
 */
void agg_free(Aggregator *a) {
    if (a == NULL) {
        return;
    }

    free(a->buckets);
    free(a);
}

/*
 * Add one stock pick into the hash map.
 */
int agg_add_pick(Aggregator *a, const char *symbol) {
    uint64_t h;
    size_t idx;

    if (a == NULL || symbol == NULL || symbol[0] == '\0') {
        return -1;
    }

    /*
     * Optional resize:
     */
    if ((a->n_distinct + 1) * 10 > a->cap * 7) {
        if (agg_rehash(a, a->cap * 2) != 0) {
            return -1;
        }
    }

    h = fnv1a(symbol);
    idx = bucket_index(h, a->cap);

    /*
     * Probe through the table until we either:
     */
    while (a->buckets[idx].state == ENTRY_OCCUPIED) {
        /*
         * If the symbol already exists, just increase its count.
         * This means another bot also picked the same ticker.
         */
        if (strcmp(a->buckets[idx].symbol, symbol) == 0) {
            a->buckets[idx].count++;
            a->total_picks++;
            return 0;
        }

        /*
         * Collision:
         */
        idx = (idx + 1) & (a->cap - 1);
    }

    /*
     * We found an empty slot, so insert a new ticker here.
     */
    strncpy(a->buckets[idx].symbol, symbol, MAX_SYMBOL_LEN - 1);
    a->buckets[idx].symbol[MAX_SYMBOL_LEN - 1] = '\0';
    a->buckets[idx].count = 1;
    a->buckets[idx].state = ENTRY_OCCUPIED;

    a->n_distinct++;
    a->total_picks++;
    return 0;
}

/* Comparator for qsort: count desc, then symbol asc. */
static int cmp_results(const void *lhs, const void *rhs) {
    const AggResult *a = (const AggResult *)lhs;
    const AggResult *b = (const AggResult *)rhs;

    if (a->count != b->count) {
        return b->count - a->count;
    }

    return strcmp(a->symbol, b->symbol);
}

/* Extract the top-k most-picked tickers, sorted by count desc. */
int agg_top_k(Aggregator *a, AggResult *out, int k) {
    AggResult *tmp;
    size_t pos = 0;
    int written;

    if (a == NULL || out == NULL || k <= 0) {
        return 0;
    }

    if (a->n_distinct == 0) {
        return 0;
    }

    tmp = malloc(a->n_distinct * sizeof(AggResult));
    if (tmp == NULL) {
        return 0;
    }

    for (size_t i = 0; i < a->cap; i++) {
        if (a->buckets[i].state != ENTRY_OCCUPIED) {
            continue;
        }

        strncpy(tmp[pos].symbol, a->buckets[i].symbol, MAX_SYMBOL_LEN - 1);
        tmp[pos].symbol[MAX_SYMBOL_LEN - 1] = '\0';
        tmp[pos].count = a->buckets[i].count;
        pos++;
    }

    qsort(tmp, pos, sizeof(AggResult), cmp_results);

    written = (k < (int)pos) ? k : (int)pos;
    memcpy(out, tmp, (size_t)written * sizeof(AggResult));

    free(tmp);
    return written;
}

/* Return the number of distinct tickers in the aggregator. */
size_t agg_distinct_count(const Aggregator *a) {
    if (a == NULL) return 0;
    return a->n_distinct;
}

/* Return the total number of picks processed. */
size_t agg_total_picks(const Aggregator *a) {
    if (a == NULL) return 0;
    return a->total_picks;
}
