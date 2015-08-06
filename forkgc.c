/*
Copyright (c) 2015 ThreadScan authors

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

#define _GNU_SOURCE // For pthread_yield().
#include "alloc.h"
#include <assert.h>
#include "child.h"
#include <fcntl.h>
#include "forkgc.h"
#include <malloc.h>
#include "proc.h"
#include <pthread.h>
#include "queue.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PIPE_READ 0
#define PIPE_WRITE 1

#ifndef NDEBUG
#define assert_monotonicity(a, n)                       \
    __assert_monotonicity(a, n, __FILE__, __LINE__)
static void __assert_monotonicity (size_t *a, int n, const char *f, int line)
{
    size_t last = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (a[i] <= last) {
            threadscan_diagnostic("Error at %s:%d\n", f, line);
            threadscan_fatal("The list is not monotonic at position %d "
                             "out of %d (%llu, last: %llu)\n",
                             i, n, a[i], last);
        }
        last = a[i];
    }
}
#else
#define assert_monotonicity(a, b) /* nothing. */
#endif

// For signaling the garbage collector with work to do.
static pthread_mutex_t g_gc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gc_cond = PTHREAD_COND_INITIALIZER;

static gc_data_t *g_gc_data, *g_uncollected_data;
static volatile int g_received_signal;
static volatile size_t g_cleanup_counter;
static int g_gc_waiting;
static size_t g_scan_max;
static pid_t child_pid;

typedef struct unref_config_t unref_config_t;

struct unref_config_t
{
    gc_data_t *gc_data;
    size_t min_val, max_val;
};

static void unref_addr (unref_config_t *unref_config, int n, int max_depth)
{
    int i;
    size_t *addrs = unref_config->gc_data->addrs;
    size_t addr = addrs[n];
    assert(addr & 1);
    size_t *p = (size_t*)PTR_MASK(addr);
    int elements = unref_config->gc_data->alloc_sz[n] / sizeof(size_t);

    for (i = 0; i < elements; ++i) {
        size_t deep_addr = PTR_MASK(p[i]);
        if (deep_addr >= unref_config->min_val
            && deep_addr <= unref_config->max_val) {

            // Found a value within our range of addresses.  See if it's in
            // our set.  Also, null it.
            p[i] = 0;
            int loc = deep_addr < addr
                ? binary_search(deep_addr, addrs, 0, n)
                : binary_search(deep_addr, addrs, n,
                                unref_config->gc_data->n_addrs);

            if (is_ref(unref_config->gc_data, loc, deep_addr)) {

                // Found an apparent address.  Unreference it.
                __sync_fetch_and_sub(&unref_config->gc_data->refs[loc], 1);
                int remaining_refs = unref_config->gc_data->refs[loc];
                assert(remaining_refs >= 0);
                if (max_depth > 0
                    && remaining_refs == 0
                    && BCAS(&unref_config->gc_data->addrs[loc],
                            deep_addr, deep_addr | 1)) {

                    // Recurse, if depth permits.  We have a max depth
                    // parameter because in certain cases, the stack could
                    // overflow.
                    unref_addr(unref_config, loc, max_depth - 1);
                }
            }
        }
    }
    free(p); // Done with it!  Bam!
}

typedef struct address_range_arg_t address_range_arg_t;

struct address_range_arg_t
{
    unref_config_t *unref_config;
    int range_begin, range_end;
};

static void *address_range (void *arg)
{
    address_range_arg_t *in = (address_range_arg_t*)arg;
    gc_data_t *gc_data = in->unref_config->gc_data;
    int i;
    for (i = in->range_begin; i < in->range_end; ++i) {
        size_t addr = gc_data->addrs[i];
        assert(addr != 0);
        assert(gc_data->refs[i] >= 0);
        if (0 == (addr & 1) && gc_data->refs[i] == 0) {
            if (BCAS(&gc_data->addrs[i], addr, addr | 1)) {
                unref_addr(in->unref_config, i, 30);
            }
        }
    }
    return NULL;
}

#define MAX_THREADS 80
#define ADDRS_PER_THREAD (128 * 1024)

static int find_unreferenced_nodes (gc_data_t *gc_data, queue_t *commq)
{
    pthread_t threads[MAX_THREADS];
    address_range_arg_t ara[MAX_THREADS];
    unref_config_t unref_config;
    int thread_count;
    int addrs_per_thread;
    int i;

    unref_config.gc_data = gc_data;
    unref_config.min_val = gc_data->addrs[0];
    // FIXME: max_val should change in the case of DEEP_REFERENCES.
    unref_config.max_val = gc_data->addrs[gc_data->n_addrs - 1];

    // Configure threads.
    thread_count = (gc_data->n_addrs / ADDRS_PER_THREAD) + 1;
    assert(thread_count > 0);
    if (thread_count > MAX_THREADS) thread_count = MAX_THREADS;
    addrs_per_thread = gc_data->n_addrs / thread_count;

    for (i = 0; i < thread_count; ++i) {
        ara[i].unref_config = &unref_config;
        ara[i].range_begin = i * addrs_per_thread;
        ara[i].range_end = (i + 1) * addrs_per_thread;
    }
    ara[thread_count - 1].range_end = gc_data->n_addrs;

    // Start the threads.
    for (i = 0; i < thread_count; ++i) {
        // FIXME: orig_* functions should get passed in.
        extern int (*orig_pthread_create) (pthread_t *, const pthread_attr_t *,
                                           void *(*) (void *), void *);
        if (orig_pthread_create(&threads[i], NULL, address_range, &ara[i])) {
            threadscan_fatal("Child was unable to create a thread.\n");
        }
    }

    // Wait for threads to return.
    for (i = 0; i < thread_count; ++i) {
        extern int (*orig_pthread_join) (pthread_t, void **);
        if (orig_pthread_join(threads[i], NULL)) {
            threadscan_fatal("Child failed to join a thread.\n");
        }
    }

    // Compact the list.
    int write_position = 0;
    int savings = 0;
    for (i = 0; i < gc_data->n_addrs; ++i) {
        if (gc_data->addrs[i] & 1) ++savings;
        else {
            // Address doesn't have its low bit set: still alive.
            if (write_position != i) {
                gc_data->addrs[write_position] = gc_data->addrs[i];
                gc_data->refs[write_position] = gc_data->refs[i];
                gc_data->alloc_sz[write_position] = gc_data->alloc_sz[i];
            }
            ++write_position;
        }
    }
    gc_data->n_addrs = write_position;

    return savings;
}

static void generate_minimap (gc_data_t *gc_data)
{
    size_t i;

    assert(gc_data);
    assert(gc_data->addrs);
    assert(gc_data->minimap);

    gc_data->n_minimap = 0;
    for (i = 0; i < gc_data->n_addrs; i += (PAGESIZE / sizeof(size_t))) {
        gc_data->minimap[gc_data->n_minimap] = gc_data->addrs[i];
        ++gc_data->n_minimap;
    }
}

static gc_data_t *aggregate_gc_data (gc_data_t *data_list)
{
    gc_data_t *ret, *tmp;
    size_t n_addrs = 0;
    int list_count = 0;

    tmp = data_list;
    do {
        n_addrs += tmp->n_addrs;
        ++list_count;
    } while ((tmp = tmp->next));

    assert(n_addrs != 0);

    // How many pages of memory are needed to store this many addresses?
    size_t pages_of_addrs = ((n_addrs * sizeof(size_t))
                             + PAGE_SIZE - sizeof(size_t)) / PAGE_SIZE;
    // How many pages of memory are needed to store the minimap?
    size_t pages_of_minimap = ((pages_of_addrs * sizeof(size_t))
                               + PAGE_SIZE - sizeof(size_t)) / PAGE_SIZE;
    // How many pages are needed to store the allocated size and reference
    // count arrays?
    size_t pages_of_count = ((n_addrs * sizeof(int))
                             + PAGE_SIZE - sizeof(int)) / PAGE_SIZE;
    // Total pages needed is the number of pages for the addresses, plus the
    // number of pages needed for the minimap, plus one (for the gc_data_t).
    char *p =
        (char*)threadscan_alloc_mmap_shared((pages_of_addrs     // addr array.
                                             + pages_of_minimap // minimap.
                                             + pages_of_count   // ref count.
                                             + pages_of_count   // alloc size.
                                             + 1)               // struct page.
                                            * PAGE_SIZE);

    // Perform assignments as offsets into the block that was bulk-allocated.
    size_t offset = 0;
    ret = (gc_data_t*)p;
    offset += PAGE_SIZE;

    ret->addrs = (size_t*)(p + offset);
    offset += pages_of_addrs * PAGE_SIZE;

    ret->minimap = (size_t*)(p + offset);
    offset += pages_of_minimap * PAGE_SIZE;

    ret->refs = (int*)(p + offset);
    offset += pages_of_count * PAGE_SIZE;

    ret->alloc_sz = (int*)(p + offset);

    ret->n_addrs = n_addrs;

    // Copy the addresses over.
    char *dest = (char*)ret->addrs;
    tmp = data_list;
    do {
        memcpy(dest, tmp->addrs, tmp->n_addrs * sizeof(size_t));
        dest += tmp->n_addrs * sizeof(size_t);
    } while ((tmp = tmp->next));

    // Sort the addresses and generate the minimap for the scanner.
    threadscan_util_sort(ret->addrs, ret->n_addrs);
    assert_monotonicity(ret->addrs, ret->n_addrs);
    generate_minimap(ret);

    // Get the size of each malloc'd block.
    int i;
    for (i = 0; i < ret->n_addrs; ++i) {
        assert(ret->alloc_sz[i] == 0);
        ret->alloc_sz[i] = malloc_usable_size((void*)ret->addrs[i]);
        assert(ret->alloc_sz[i] > 0);
    }

#ifndef NDEBUG
    for (i = 0; i < ret->n_addrs; ++i) {
        assert(ret->refs[i] == 0);
    }
#endif

    return ret;
}

static void garbage_collect (gc_data_t *gc_data, queue_t *commq)
{
    gc_data_t *working_data;
    int sig_count;
    int pipefd[2];

    // Include the addrs from the last collection iteration.
    if (g_uncollected_data) {
        gc_data_t *tmp = g_uncollected_data;
        while (tmp->next) tmp = tmp->next;
        tmp->next = gc_data;
        gc_data = g_uncollected_data;
    }

    working_data = aggregate_gc_data(gc_data);

    // Open a pipe for communication between parent and child.
    if (0 != pipe2(pipefd, O_DIRECT)) {
        threadscan_fatal("GC thread was unable to open a pipe.\n");
    }

    // Send out signals.  When everybody is waiting at the line, fork the
    // process for the snapshot.
    g_received_signal = 0;
    sig_count = threadscan_proc_signal(SIGTHREADSCAN);
    while (g_received_signal < sig_count) pthread_yield();
    child_pid = fork();

    if (child_pid == -1) {
        threadscan_fatal("Collection failed (fork).\n");
    } else if (child_pid == 0) {
        // Child: Scan memory, pass pointers back to the parent to free, pass
        // remaining pointers back, and exit.
        close(pipefd[PIPE_READ]);
        threadscan_child(working_data, pipefd[PIPE_WRITE]);
        close(pipefd[PIPE_WRITE]);
        exit(0);
    }

    ++g_cleanup_counter;
    close(pipefd[PIPE_WRITE]);

    // Wait for the child to complete the scan.
    size_t bytes_scanned;
    if (sizeof(size_t) != read(pipefd[PIPE_READ], &bytes_scanned,
                               sizeof(size_t))) {
        threadscan_fatal("Failed to read from child.\n");
    }
    if (bytes_scanned > g_scan_max) g_scan_max = bytes_scanned;

    // Identify unreferenced memory and free it.
    int savings;
    int iters = 0;
    do {
        ++iters;
        savings = find_unreferenced_nodes(working_data, commq);
    } while (savings > 0 && working_data->n_addrs > 0);

    gc_data->n_addrs = 0;
    int i;
    for (i = 0; i < working_data->n_addrs; ++i) {
        if (g_uncollected_data == NULL) g_uncollected_data = gc_data;
        if (gc_data->n_addrs >= gc_data->capacity) {
            gc_data = gc_data->next;
            assert(gc_data != NULL);
            gc_data->n_addrs = 0;
        }
        gc_data->addrs[gc_data->n_addrs++] = working_data->addrs[i];
    }

    close(pipefd[PIPE_READ]);
    threadscan_alloc_munmap(working_data); // FIXME: ...

    // Free up unnecessary space.
    assert(gc_data);
    gc_data_t *tmp;
    if (gc_data->n_addrs) {
        tmp = gc_data;
        gc_data = gc_data->next;
        tmp->next = NULL;
    } else assert(NULL == g_uncollected_data);
    while (gc_data) {
        tmp = gc_data->next;
        threadscan_alloc_munmap(gc_data); // FIXME: Munmap is bad.
        gc_data = tmp;
    }
}

/****************************************************************************/
/*                            Exported functions                            */
/****************************************************************************/

/**
 * Wait for the GC routine to complete its snapshot.
 */
void forkgc_wait_for_snapshot ()
{
    size_t old_counter;
    jmp_buf env; // Spilled registers.

    // Acknowledge the signal and wait for the snapshot to complete.
    old_counter = g_cleanup_counter;
    setjmp(env);
    __sync_fetch_and_add(&g_received_signal, 1);
    while (old_counter == g_cleanup_counter) pthread_yield();
}

/**
 * Pass a list of pointers to the GC thread for it to collect.
 */
void forkgc_initiate_collection (gc_data_t *gc_data)
{
    pthread_mutex_lock(&g_gc_mutex);
    gc_data->next = g_gc_data;
    g_gc_data = gc_data;
    if (g_gc_waiting != 0) {
        pthread_cond_signal(&g_gc_cond);
    }
    pthread_mutex_unlock(&g_gc_mutex);
}

/**
 * Garbage-collector thread.
 */
void *forkgc_thread (void *ignored)
{
    gc_data_t *gc_data;

    // FIXME: Warning: Fragile code knows the size of a pointer and a page.
    char *buffer = threadscan_alloc_mmap_shared(PAGE_SIZE * 9);
    queue_t *commq = (queue_t*)buffer;
    threadscan_queue_init(commq, (size_t*)&buffer[PAGE_SIZE], PAGE_SIZE);

    while ((1)) {
        pthread_mutex_lock(&g_gc_mutex);
        if (NULL == g_gc_data) {
            // Wait for somebody to come up with a set of addresses for us to
            // collect.
            g_gc_waiting = 1;
            pthread_cond_wait(&g_gc_cond, &g_gc_mutex);
            g_gc_waiting = 0;
        }

        assert(g_gc_data);
        gc_data = g_gc_data;
        g_gc_data = NULL;
        pthread_mutex_unlock(&g_gc_mutex);

#ifndef NDEBUG
        int n = 1;
        gc_data_t *tmp = gc_data;
        while (NULL != (tmp = tmp->next)) ++n;
        threadscan_diagnostic("%d collects waiting.\n", n);
#endif

        garbage_collect(gc_data, commq);
    }

    return NULL;
}

/**
 * Print program statistics to stdout.
 */
void forkgc_print_statistics ()
{
    char statm[256];
    size_t bytes_read;
    FILE *fp;

    fp = fopen("/proc/self/statm", "r");
    if (NULL == fp) {
        threadscan_fatal("Unable to open /proc/self/statm.\n");
    }
    bytes_read = fread(statm, 1, 255, fp);
    statm[statm[bytes_read - 1] == '\n'
          ? bytes_read - 1
          : bytes_read
          ] = '\0';
    fclose(fp);

    printf("statm: %s\n", statm);
    printf("fork-count: %zu\n", g_cleanup_counter);
    printf("scan-max: %zu\n", g_scan_max);
}

__attribute__((destructor))
static void process_death ()
{
    if (child_pid > 0) {
        // There's still an outstanding child.  Kill it.
        kill(child_pid, 9);
    }
}
