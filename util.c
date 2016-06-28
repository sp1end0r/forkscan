/*
Copyright (c) 2015 ForkGC authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <assert.h>
#include "alloc.h"
#include "env.h"
#include <errno.h>
#include <jemalloc/jemalloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "util.h"

/****************************************************************************/
/*                         Defines, typedefs, etc.                          */
/****************************************************************************/

// Size of a per-thread metadata memory block.
#define MEMBLOCK_SIZE PAGESIZE

typedef struct free_list_node_t free_list_node_t;

struct free_list_node_t
{
    free_list_node_t *next;
    free_t *free_list;
};

static free_list_node_t *free_list_list;
static pthread_mutex_t free_list_list_lock = PTHREAD_MUTEX_INITIALIZER;

// FIXME: Are these actually used?
static pthread_mutex_t g_staged_lock = PTHREAD_MUTEX_INITIALIZER;
static thread_data_t *g_td_staged_to_free = NULL;

/****************************************************************************/
/*                       Storage for per-thread data.                       */
/****************************************************************************/

thread_data_t *forkgc_util_thread_data_new ()
{
    char *memblock = (char*)forkgc_alloc_mmap(MEMBLOCK_SIZE);
    size_t *local_list =
        (size_t*)forkgc_alloc_mmap(g_forkgc_ptrs_per_thread
                                   * sizeof(size_t));
    thread_data_t *td = (thread_data_t*)memblock;
    forkgc_queue_init(&td->ptr_list, local_list,
                      g_forkgc_ptrs_per_thread);
    td->local_block.low = td->local_block.high = 0;
    td->ref_count = 1;
    return td;
}

void forkgc_util_thread_data_decr_ref (thread_data_t *td)
{
    if (0 == __sync_fetch_and_sub(&td->ref_count, 1) - 1) {
        pthread_mutex_lock(&g_staged_lock);
        td->next = g_td_staged_to_free;
        g_td_staged_to_free = td;
        pthread_mutex_unlock(&g_staged_lock);
    }
}

void forkgc_util_thread_data_free (thread_data_t *td)
{
    assert(td);
    assert(td->ref_count == 0);

    // FIXME: Should do something about any possible remaining pointers in this
    // thread's ptr_list!  Right now, they're getting leaked.
    forkgc_alloc_munmap(td->ptr_list.e);

    forkgc_alloc_munmap(td);
}

void forkgc_util_thread_data_cleanup (pthread_t tid)
{
    thread_data_t *td, *last = NULL;

    // Find the thread data and remove it from the list.
    pthread_mutex_lock(&g_staged_lock);
    td = g_td_staged_to_free;
    assert(td);
    while (0 == pthread_equal(td->self, tid)) {
        last = td;
        td = td->next;
        assert(td);
    }
    if (last) {
        last->next = td->next;
    } else {
        g_td_staged_to_free = td->next;
    }
    pthread_mutex_unlock(&g_staged_lock);

    if (td->ref_count > 0) {
        forkgc_fatal("ForkGC: "
                     "detected data race on exiting thread.\n");
    }

    if (td->stack_is_ours) {
        forkgc_alloc_munmap(td->user_stack_low);
    }

    forkgc_util_thread_data_free(td);
}

void forkgc_util_thread_list_init (thread_list_t *tl)
{
    assert(tl);
    if (tl->head == NULL) {
        // Do not reinitialize the mutex.  That would be bad.
        pthread_mutex_init(&tl->lock, NULL);
        tl->count = 0;
    }
}

void forkgc_util_thread_list_add (thread_list_t *tl, thread_data_t *td)
{
    assert(tl); assert(td);
    pthread_mutex_lock(&tl->lock);
    td->next = tl->head;
    tl->head = td;
    ++tl->count;
    pthread_mutex_unlock(&tl->lock);
}

void forkgc_util_thread_list_remove (thread_list_t *tl, thread_data_t *td)
{
    thread_data_t *tmp;
    assert(tl); assert(td);
    pthread_mutex_lock(&tl->lock);
    tmp = tl->head;
    assert(tmp);
    if (tmp == td) {
        tl->head = td->next;
    } else {
        while (tmp->next != td) {
            tmp = tmp->next;
            assert(NULL != tmp);
        }
        tmp->next = td->next;
    }
    assert(tl->count > 0);
    --tl->count;
    pthread_mutex_unlock(&tl->lock);
}

thread_data_t *forkgc_util_thread_list_find (thread_list_t *tl, size_t addr)
{
    thread_data_t *ret;

    pthread_mutex_lock(&tl->lock);
    for (ret = tl->head; ret != NULL; ret = ret->next) {
        if (addr >= (size_t)ret->user_stack_low
            && addr < (size_t)ret->user_stack_high) {
            __sync_fetch_and_add(&ret->ref_count, 1);
            break;
        }
    }
    pthread_mutex_unlock(&tl->lock);

    return ret;
}

void forkgc_util_push_free_list (free_t *free_list)
{
    // FIXME: We should really do this add/remove stuff with transactions.
    free_list_node_t *node = MALLOC(sizeof(free_list_node_t));
    pthread_mutex_lock(&free_list_list_lock);
    node->free_list = free_list;
    node->next = free_list_list;
    free_list_list = node;
    pthread_mutex_unlock(&free_list_list_lock);
}

free_t *forkgc_util_pop_free_list ()
{
    // FIXME: We should really do this add/remove stuff with transactions.
    free_list_node_t *node;
    free_t *free_list = NULL;
    if (free_list_list == NULL) return NULL;
    pthread_mutex_lock(&free_list_list_lock);
    node = free_list_list;
    if (node != NULL) free_list_list = node->next;
    pthread_mutex_unlock(&free_list_list_lock);
    if (node) {
        free_list = node->free_list;
        FREE(node);
    }
    return free_list;
}

/****************************************************************************/
/*                              I/O functions.                              */
/****************************************************************************/

int forkgc_diagnostic (const char *format, ...)
{
    va_list arg;
    int ret;

    assert(format);

    fprintf(stderr, "ForkGC diagnostic: ");
    va_start(arg, format);
    ret = vfprintf(stderr, format, arg);
    va_end(arg);

    return ret;
}

void forkgc_fatal (const char *format, ...)
{
    va_list arg;

    assert(format);

    printf("ForkGC fatal: ");
    va_start(arg, format);
    vfprintf(stderr, format, arg);
    va_end(arg);

    assert(0);
    exit(1);
}

/****************************************************************************/
/*                              Sort utility.                               */
/****************************************************************************/

static void swap (size_t *addrs, int n, int m)
{
    size_t addr = addrs[n];
    addrs[n] = addrs[m];
    addrs[m] = addr;
}

static int partition (size_t *addrs, int min, int max)
{
    int pivot = (max + min) / 2;
    size_t pivot_val = addrs[pivot];
    int mid = min;
    int i;

    swap(addrs, pivot, max);
    for (i = min; i < max; ++i) {
        if (addrs[i] <= pivot_val) {
            swap(addrs, i, mid);
            ++mid;
        }
    }
    swap(addrs, mid, max);
    return mid;
}

static void insertion_sort (size_t *addrs, int min, int max)
{
    int i, j;
    for (i = min + 1; i <= max; ++i) {
        for (j = i; j > 0 && addrs[j - 1] > addrs[j]; --j) {
            swap(addrs, j, j - 1);
        }
    }
}

#define SORT_THRESHOLD 16

/**
 * Standard quicksort for the working pointers arrays: [min, max]
 */
static void quicksort (size_t *addrs, int min, int max)
{
    if (max - min > SORT_THRESHOLD) {
        int mid = partition(addrs, min, max);
        quicksort(addrs, min, mid - 1);
        quicksort(addrs, mid + 1, max);
    } else {
        insertion_sort(addrs, min, max);
    }
}

/**
 * Sort the array, a, of the given length from lowest to highest.  The sort
 * happens in-place.
 */
void forkgc_util_sort (size_t *a, int length)
{
    quicksort(a, 0, length - 1);
}

/**
 * Randomize the ordering of an array of addrs (of length n) in place.
 */
void forkgc_util_randomize (size_t *addrs, int n)
{
    unsigned int i;
    for (i = 0; i < n; ++i) {
        unsigned int tmp = (i * 2147483647) % n;
        if (i != tmp) {
            swap(addrs, i, tmp);
        }
    }
}

/**
 * Compact a sorted list with duplicates and return the savings.
 */
int forkgc_util_compact (size_t *a, int length)
{
    int search, write = 0;

    if (length < 2) return 0;

    for (search = 1; search < length; ++search) {
        if (a[search] == a[write]) continue;
        ++write;
        if (write < search) {
            a[write] = a[search];
        }
    }

    ++write;
    return length - write;
}

/**
 * Get a timestamp in ms.
 */
size_t forkscan_rdtsc ()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    size_t ret = (size_t)(ts.tv_sec * (1000));
    ret += (size_t)(ts.tv_nsec / (1000 * 1000));
    return ret;
}
