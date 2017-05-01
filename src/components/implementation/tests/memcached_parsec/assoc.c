/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
 */

#include "memcached.h"
#include "micro_booter.h"
#include "mem_mgr.h"

typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */

struct list_head {
    item *head;
    char pad[CACHE_LINE-sizeof(item *)];
} __attribute__((aligned(CACHE_LINE), packed));
/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;

/* Main hash table. This is where we look except during expansion. */
static struct list_head *primary_hashtable;

/* Number of items in the hash table. */
static unsigned int hash_items = 0;

void
assoc_init(int node, const int hashtable_init)
{
    int size = hashsize(HASHPOWER_DEFAULT) * sizeof(struct list_head);
    size = round_up_to_page(size)/PAGE_SIZE;
    primary_hashtable = alloc_pages(size);
    memset(primary_hashtable, 0, size*PAGE_SIZE);
    return ;
}

item *
assoc_find(int node, const char *key, const size_t nkey, const uint32_t hv)
{
    item *it;

    it = primary_hashtable[hv & hashmask(hashpower)].head;

    item *ret = NULL;
    while (it) {
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
    }
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */

static item** 
_hashitem_before(int node, const char *key, const size_t nkey, const uint32_t hv)
{
    item **pos;

    pos = &(primary_hashtable[hv & hashmask(hashpower)].head);
    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }

    return pos;
}

/* Note: this isn't an assoc_update.  The key must not already exist to call this */
int
assoc_insert(int node, item *it, const uint32_t hv)
{
    assert((hv & hashmask(hashpower)) % (NUM_NODE/2) == (uint32_t)node);
    it->h_next = primary_hashtable[hv & hashmask(hashpower)].head;
    clwb_range(it, ((char *)it)+MC_SLAB_OBJ_SZ);
    primary_hashtable[hv & hashmask(hashpower)].head = it;
    cos_wb_cache(&(primary_hashtable[hv & hashmask(hashpower)].head));

    return 1;
}

void
assoc_delete(int node, const char *key, const size_t nkey, const uint32_t hv)
{
    item **before = _hashitem_before(node, key, nkey, hv);
    assert((hv & hashmask(hashpower)) % (NUM_NODE/2) == (uint32_t)node);
    if (*before) {
        item *nxt;
        nxt = (*before)->h_next;
        *before = nxt;
        cos_wb_cache(before);
        return;
    }
    assert(*before != 0);
}

void
assoc_flush(const uint32_t hv)
{
    item **pos;

    pos = &(primary_hashtable[hv & hashmask(hashpower)].head);
    cos_flush_cache(pos);
    while (*pos) {
        pos = &(*pos)->h_next;
        cos_flush_cache(pos);
    }
}

