#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SIZE 4096 /* FIXME: set at runtime */

typedef struct {
    int cnt;                /* actual pool count */
    int pal;                /* pool array length (2^x ceil of cnt) */
    int min_pool, max_pool; /* minimum/maximum pool size */
    void **ps;              /* pools */
    int *sizes;             /* chunk size for each pool */
    void *hs[1];            /* heads for pools' free lists */
} mpool;

static void *get_mmap(long sz)
{
    void *p =
        mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    return p;
}

/* base-2 integer ceiling */
static unsigned int iceil2(unsigned int x)
{
    x = x - 1;
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >> 16);
    return X + 1;
}

/* mmap a new memory pool of TOTAL_SZ bytes, then build an internal
 * freelist of SZ-byte cells, with the head at (result)[0].
 * Returns NULL on error.
 */
void **mpool_new_pool(unsigned int sz, unsigned int total_sz)
{
    int o = 0; /* o=offset */
    void *p = get_mmap(sz > total_sz ? sz : total_sz);
    if (!p)
        return NULL;
    int **pool = (int **) p;
    assert(pool);
    assert(sz > sizeof(void *));

    void *last = NULL;
    int lim = (total_sz / sz);
    for (int i = 0; i < lim; i++) {
        if (last)
            assert(last == pool[o]);
        o = (i * sz) / sizeof(void *);
        pool[o] = (int *) &pool[o + (sz / sizeof(void *))];
        last = pool[o];
    }
    pool[o] = NULL;
    return p;
}

/* Add a new pool, resizing the pool array if necessary. */
static int add_pool(mpool *mp, void *p, int sz)
{
    assert(p);
    assert(sz > 0);
    if (mp->cnt == mp->pal) {
        mp->pal *= 2; /* RAM will exhaust before overflow */
        void **nps = realloc(mp->ps, mp->pal * sizeof(void **));
        void *nsizes = realloc(mp->sizes, mp->pal * sizeof(int *));
        if (!nps || !nsizes)
            return -1;
        mp->sizes = nsizes;
        mp->ps = nps;
    }

    mp->ps[mp->cnt] = p;
    mp->sizes[mp->cnt] = sz;
    mp->cnt++;
    return 0;
}

/* Initialize a memory pool for allocations between 2^min2 and 2^max2,
 * inclusive. Larger allocations will be directly allocated and freed
 * via mmap / munmap.
 * Returns NULL on error.
 */
mpool *mpool_init(int min2, int max2)
{
    int cnt = max2 - min2 + 1; /* pool array count */
    int palen = iceil2(cnt);

    assert(cnt > 0);
    mpool *mp = malloc(sizeof(mpool) + (cnt - 1) * sizeof(void *));
    void *pools = malloc(palen * sizeof(void **));
    int *sizes = malloc(palen * sizeof(int));
    if (!mp || !pools || !sizes)
        return NULL;

    mp->cnt = cnt;
    mp->ps = pools;
    mp->pal = palen;
    mp->sizes = sizes;
    mp->min_pool = (1 << min2), mp->max_pool = (1 << max2);
    memset(sizes, 0, palen * sizeof(int));
    memset(pools, 0, palen * sizeof(void *));
    memset(mp->hs, 0, cnt * sizeof(void *));

    return mp;
}

/* Free a memory pool set. */
void mpool_free(mpool *mp)
{
    assert(mp);
    for (long i = 0; i < mp->cnt; i++) {
        void *p = mp->ps[i];
        if (!p)
            continue;
        long sz = mp->sizes[i];
        assert(sz > 0);
        if (sz < PAGE_SIZE)
            sz = PAGE_SIZE;
        if (munmap(mp->ps[i], sz) == -1)
            fprintf(stderr, "Failed to unmap %ld bytes at %p\n",
                    sz, mp->ps[i]);
    }
    free(mp->ps);
    free(mp);
}

/* Allocate memory out of the relevant memory pool.
 * If larger than max_pool, just mmap it. If pool is full, mmap a new one and
 * link it to the end of the current one.
 * Returns NULL on error.
 */
void *mpool_alloc(mpool *mp, int sz)
{
    void **cur;
    int i, p;
    assert(mp);
    if (sz >= mp->max_pool) {
        cur = get_mmap(sz); /* just mmap it */
        if (!cur)
            return NULL;
        return cur;
    }

    long szceil = 0;
    for (i = 0, p = mp->min_pool;; i++, p *= 2) {
        if (p > sz) {
            szceil = p;
            break;
        }
    }
    assert(szceil > 0);

    cur = mp->hs[i]; /* get current head */
    if (!cur) {      /* lazily allocate and initialize pool */
        void **pool = mpool_new_pool(szceil, PAGE_SIZE);
        if (!pool)
            return NULL;
        mp->ps[i] = mp->hs[i] = pool;
        mp->sizes[i] = szceil;
        cur = mp->hs[i];
    }
    assert(cur);

    if (!(*cur)) { /* if at end, attach to a new page */
        void **np = mpool_new_pool(szceil, PAGE_SIZE);
        if (!np)
            return NULL;
        *cur = &np[0];
        assert(*cur);
        if (add_pool(mp, np, szceil) < 0)
            return NULL;
    }
    assert(*cur > (void *) PAGE_SIZE);

    mp->hs[i] = *cur; /* set head to next head */
    return cur;
}

/* Push an individual pointer P back on the freelist for the pool with size
 * SZ cells. if SZ is > the max pool size, just munmap it.
 * Return pointer P (SZ bytes in size) to the appropriate pool.
 */
void mpool_repool(mpool *mp, void *p, int sz)
{
    int i = 0;

    if (sz > mp->max_pool) {
        if (munmap(p, sz) == -1)
            fprintf(stderr, "Failed to unmap %d bytes at %p\n", sz, p);
        return;
    }

    void **ip = (void **) p;
    *ip = mp->hs[i];
    assert(ip);
    mp->hs[i] = ip;
}
// 對應的測試程式如下:

#define PMAX 11

int main(void)
{
    /* Initialize a new mpool for values 2^4 to 2^PMAX */
    mpool *mp = mpool_init(4, PMAX);

    srandom(getpid() ^ (intptr_t) &main); /* Assume ASLR */

    for (int i = 0; i < 5000000; i++) {
        int sz = random() % 64;
        /* also alloc some larger chunks  */
        if (random() % 100 == 0)
            sz = random() % 10000;
        if (!sz)
            sz = 1; /* avoid zero allocations */
        int *ip = (int *) mpool_alloc(mp, sz);
        *ip = 7;

        /* randomly repool some of them */
        if (random() % 10 == 0) /* repool, known size */
            mpool_repool(mp, ip, sz);
        if (i % 10000 == 0 && i > 0) {
            putchar('.');
            if (i % 700000 == 0)
                putchar('\n');
        }
    }

    mpool_free(mp);
    putchar('\n');
    return 0;
}
