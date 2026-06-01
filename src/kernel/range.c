#include "range.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"

/*
 * Merge ranges: sort by start, then merge adjacent/overlapping.
 * Caller should ensure ranges[0..*nr-1] are valid.
 */
void range_sort(struct range *ranges, int *nr)
{
    int i, j;

    if (*nr < 2)
        return;

    /* Simple insertion sort by start */
    for (i = 1; i < *nr; i++) {
        struct range key = ranges[i];
        j = i - 1;
        while (j >= 0 && ranges[j].start > key.start) {
            ranges[j + 1] = ranges[j];
            j--;
        }
        ranges[j + 1] = key;
    }

    /* Merge adjacent/overlapping ranges */
    int idx = 0;
    for (i = 1; i < *nr; i++) {
        if (ranges[idx].end >= ranges[i].start - 1) {
            /* Overlap or adjacent – merge */
            if (ranges[i].end > ranges[idx].end)
                ranges[idx].end = ranges[i].end;
        } else {
            idx++;
            if (idx != i)
                ranges[idx] = ranges[i];
        }
    }
    *nr = idx + 1;
}

int range_add(struct range *ranges, int *nr, int max_size,
              uint64_t start, uint64_t end)
{
    if (start > end)
        return -EINVAL;

    /* First, subtract the new interval to avoid overlap, then add it back */
    int ret = range_remove(ranges, nr, start, end);
    if (ret < 0)
        return ret;

    if (*nr >= max_size)
        return -ENOMEM;

    ranges[*nr].start = start;
    ranges[*nr].end   = end;
    (*nr)++;

    range_sort(ranges, nr);
    return *nr;
}

int range_remove(struct range *ranges, int *nr, uint64_t start, uint64_t end)
{
    struct range tmp[64];  /* local buffer for fragments */
    int tmp_nr = 0;
    int i;

    if (start > end)
        return -EINVAL;

    for (i = 0; i < *nr; i++) {
        uint64_t rstart = ranges[i].start;
        uint64_t rend   = ranges[i].end;

        /* No overlap */
        if (rend < start || rstart > end) {
            tmp[tmp_nr++] = ranges[i];
            continue;
        }

        /* Left fragment */
        if (rstart < start) {
            tmp[tmp_nr].start = rstart;
            tmp[tmp_nr].end   = start - 1;
            tmp_nr++;
        }

        /* Right fragment */
        if (rend > end) {
            tmp[tmp_nr].start = end + 1;
            tmp[tmp_nr].end   = rend;
            tmp_nr++;
        }
    }

    memcpy(ranges, tmp, sizeof(struct range) * tmp_nr);
    *nr = tmp_nr;
    return *nr;
}

int range_contains(struct range *ranges, int nr, uint64_t val)
{
    int lo = 0, hi = nr - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (val < ranges[mid].start) {
            hi = mid - 1;
        } else if (val > ranges[mid].end) {
            lo = mid + 1;
        } else {
            return 1;
        }
    }
    return 0;
}

int range_overlaps(struct range *ranges, int nr,
                   uint64_t start, uint64_t end)
{
    int i;
    for (i = 0; i < nr; i++) {
        if (ranges[i].start <= end && ranges[i].end >= start)
            return 1;
    }
    return 0;
}

void range_init(void)
{
    kprintf("[OK] range: Range management initialised\n");
}
