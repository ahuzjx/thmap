/*
 * Copyright (c) 2018 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

/*
 * Concurrent trie-hash map.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>

#include "thmap.h"
#include "utils.h"

/*
 * The root level fanout is 64 (indexed using the first 6 bits),
 * while each subsequent level has a fanout of 16 (using 4 bits).
 * The hash function produces 32-bit values.
 */

#define	ROOT_BITS	(6)
#define	ROOT_SIZE	(1 << ROOT_BITS)
#define	ROOT_MASK	(ROOT_SIZE - 1)

#define	LEVEL_BITS	(4)
#define	LEVEL_SIZE	(1 << LEVEL_BITS)
#define	LEVEL_MASK	(LEVEL_SIZE - 1)

#define	HASHVAL_BITS	(32)
#define	HASHVAL_SHIFT	(5)
#define	HASHVAL_MASK	(HASHVAL_BITS - 1)

/*
 * Instead of raw pointers, we use offsets from the base address.
 * This accommodates the use of this data structure in shared memory,
 * where mappings can be in different address spaces.
 *
 * The pointers must be aligned, since pointer tagging is used to
 * differentiate the intermediate nodes from leaves.  We reserve the
 * least significant bit.
 */
typedef uintptr_t thmap_ptr_t;

#define	THMAP_LEAF_BIT		(0x1)

#define	THMAP_ALIGNED_P(p)	(((uintptr_t)(p) & 3) == 0)
#define	THMAP_ALIGN(p)		((uintptr_t)(p) & ~(uintptr_t)3)
#define	THMAP_INODE_P(p)	(((uintptr_t)(p) & THMAP_LEAF_BIT) == 0)

#define	THMAP_GETPTR(th, p)	((void *)((th)->baseptr + (uintptr_t)(p)))
#define	THMAP_GETOFF(th, p)	((thmap_ptr_t)((uintptr_t)(p) - (th)->baseptr))
#define	THMAP_NODE(th, p)	THMAP_GETPTR(th, THMAP_ALIGN(p))

/*
 * State field.
 */

#define	NODE_LOCKED		(1U << 31)		// lock (writers)
#define	NODE_DELETED		(1U << 30)		// node deleted
#define	NODE_COUNT(s)		((s) & 0x3fffffff)	// slot count mask

/*
 * There are two types of nodes:
 * - Intermediate nodes -- arrays pointing to another level or a leaf;
 * - Leaves, which store a key-value pair.
 */

typedef struct {
	uint32_t	state;
	thmap_ptr_t	parent;
	thmap_ptr_t	slots[];
} thmap_inode_t;

#define	THMAP_ROOT_LEN		offsetof(thmap_inode_t, slots[ROOT_SIZE])
#define	THMAP_INODE_LEN		offsetof(thmap_inode_t, slots[LEVEL_SIZE])

typedef struct {
	thmap_ptr_t	key;
	size_t		len;
	void *		val;
} thmap_leaf_t;

#define	THMAP_QUERY_INIT(l)	{ .level = (l), .hashidx = -1, .hashval = 0 }

typedef struct {
	unsigned	level;		// current level in the tree
	int		hashidx;	// current hash index (block of bits)
	uint64_t	hashval;	// current hash value
} thmap_query_t;

typedef struct {
	uintptr_t	addr;
	size_t		len;
	void *		next;
} thmap_gc_t;

struct thmap {
	uintptr_t	baseptr;
	thmap_inode_t *	root;
	unsigned	flags;

	const thmap_ops_t *ops;
	thmap_gc_t *	gc_list;
};

static void	stage_mem_gc(thmap_t *, uintptr_t, size_t);

/*
 * A few low-level helper routines.
 */

static uintptr_t
alloc_wrapper(size_t len)
{
	return (uintptr_t)malloc(len);
}

static void
free_wrapper(uintptr_t addr, size_t len)
{
	free((void *)addr); (void)len;
}

static const thmap_ops_t thmap_default_ops = {
	.alloc = alloc_wrapper,
	.free = free_wrapper
};

/*
 * NODE LOCKING.
 */

static inline bool
node_locked_p(const thmap_inode_t *node)
{
	return (node->state & NODE_LOCKED) != 0;
}

static void
lock_node(thmap_inode_t *node)
{
	unsigned bcount = SPINLOCK_BACKOFF_MIN;
	uint32_t s;
again:
	s = node->state;
	if (s & NODE_LOCKED) {
		SPINLOCK_BACKOFF(bcount);
		goto again;
	}
	/*
	 * CAS will issue a full memory fence for us.
	 *
	 * WARNING: for optimisations purposes, callers rely on us
	 * issuing load and store fence
	 */
	if (!atomic_compare_exchange_weak(&node->state, s, s | NODE_LOCKED)) {
		bcount = SPINLOCK_BACKOFF_MIN;
		goto again;
	}
}

static void
unlock_node(thmap_inode_t *node)
{
	uint32_t s = node->state & ~NODE_LOCKED;

	ASSERT(node_locked_p(node));
	atomic_thread_fence(memory_order_stores);
	node->state = s; // atomic store
}

/*
 * HASH VALUE AND KEY OPERATIONS.
 */

/*
 * hashval_getslot: given the key, compute the hash (if not already cached)
 * and return the offset for the current level.
 */
static unsigned
hashval_getslot(thmap_query_t *query, const void * restrict key, size_t len)
{
	const unsigned level = query->level;
	unsigned nbits = ROOT_BITS + (level * LEVEL_BITS);
	const int i = nbits >> HASHVAL_SHIFT;

	/* Count the bit offset. */
	if (query->hashidx != i) {
		/* Generate a hash value for a required range. */
		query->hashval = murmurhash3(key, len, i);
		query->hashidx = i;
	}
	if (level == 0) {
		/* Root level has a different fanout. */
		return query->hashval & ROOT_MASK;
	}
	nbits = roundup2(nbits, LEVEL_BITS) & HASHVAL_MASK;
	return (query->hashval >> nbits) & LEVEL_MASK;
}

static unsigned
hashval_getleafslot(const thmap_t *thmap,
    const thmap_leaf_t *leaf, unsigned level)
{
	thmap_query_t query = THMAP_QUERY_INIT(level);
	const void *key = THMAP_GETPTR(thmap, leaf->key);
	return hashval_getslot(&query, key, leaf->len);
}

static bool
key_cmp_p(const thmap_t *thmap, const thmap_leaf_t *leaf,
    const void * restrict key, size_t len)
{
	const void *leafkey = THMAP_GETPTR(thmap, leaf->key);
	return len == leaf->len && memcmp(key, leafkey, len) == 0;
}

/*
 * INTER-NODE OPERATIONS.
 */

static thmap_inode_t *
node_create(thmap_t *thmap, thmap_inode_t *parent)
{
	size_t len = offsetof(thmap_inode_t, slots[LEVEL_SIZE]);
	thmap_inode_t *node;
	uintptr_t p;

	ASSERT(parent);

	p = thmap->ops->alloc(len);
	if (!p) {
		return NULL;
	}
	node = THMAP_GETPTR(thmap, p);
	ASSERT(THMAP_ALIGNED_P(node));

	memset(node, 0, len);
	node->state = NODE_LOCKED;
	node->parent = THMAP_GETOFF(thmap, parent);
	return node;
}

static void
node_insert(thmap_inode_t *node, unsigned slot, thmap_ptr_t child)
{
	ASSERT(node_locked_p(node));
	ASSERT((node->state & NODE_DELETED) == 0);
	ASSERT(node->slots[slot] == 0);

	ASSERT(NODE_COUNT(node->state) < ROOT_SIZE);

	node->slots[slot] = child;
	node->state++;
}

static void
node_remove(thmap_inode_t *node, unsigned slot)
{
	ASSERT(node_locked_p(node));
	ASSERT((node->state & NODE_DELETED) == 0);
	ASSERT(node->slots[slot] != 0);

	ASSERT(NODE_COUNT(node->state) > 0);
	ASSERT(NODE_COUNT(node->state) <= ROOT_SIZE);

	node->slots[slot] = 0;
	node->state--;
}

/*
 * LEAF OPERATIONS.
 */

static thmap_leaf_t *
leaf_create(const thmap_t *thmap, const void *key, size_t len, void *val)
{
	thmap_leaf_t *leaf;
	uintptr_t leaf_off, key_off;

	leaf_off = thmap->ops->alloc(sizeof(thmap_leaf_t));
	if (!leaf_off) {
		return NULL;
	}
	leaf = THMAP_GETPTR(thmap, leaf_off);
	ASSERT(THMAP_ALIGNED_P(leaf));

	if ((thmap->flags & THMAP_NOCOPY) == 0) {
		/*
		 * Copy the key.
		 */
		key_off = thmap->ops->alloc(len);
		if (!key_off) {
			thmap->ops->free(leaf_off, sizeof(thmap_leaf_t));
			return NULL;
		}
		memcpy(THMAP_GETPTR(thmap, key_off), key, len);
		leaf->key = key_off;
	} else {
		/* Otherwise, we use a reference. */
		leaf->key = (uintptr_t)key;
	}
	leaf->len = len;
	leaf->val = val;
	return leaf;
}

static void *
leaf_free(const thmap_t *thmap, thmap_leaf_t *leaf)
{
	void *val = leaf->val;

	if ((thmap->flags & THMAP_NOCOPY) == 0) {
		thmap->ops->free(leaf->key, leaf->len);
	}
	thmap->ops->free(THMAP_GETOFF(thmap, leaf), sizeof(thmap_leaf_t));
	return val;
}

static thmap_leaf_t *
get_leaf(const thmap_t *thmap, thmap_inode_t *parent, unsigned slot)
{
	thmap_ptr_t node;

	node = parent->slots[slot];
	if (THMAP_INODE_P(node)) {
		return NULL;
	}
	return THMAP_NODE(thmap, node);
}

/*
 * find_edge_node: given the hash, traverse the tree to find the edge node.
 *
 * => Returns an aligned (clean) pointer to the parent node.
 * => Returns the slot number and sets current level.
 */
static thmap_inode_t *
find_edge_node(const thmap_t *thmap, thmap_query_t *query,
    const void * restrict key, size_t len, unsigned *slot)
{
	thmap_inode_t *parent = thmap->root;
	thmap_ptr_t node;
	unsigned off;

	/* Root level has a different fanout. */
	ASSERT(query->level == 0);
	off = hashval_getslot(query, key, len);
	node = parent->slots[off];

	/* Descend the tree until we find a leaf or empty slot. */
	while (node && THMAP_INODE_P(node)) {
		query->level++;
		off = hashval_getslot(query, key, len);
		parent = THMAP_NODE(thmap, node);

		/* Ensure the parent load happens before the child load. */
		atomic_thread_fence(memory_order_loads);
		node = parent->slots[off];
	}
	atomic_thread_fence(memory_order_loads);
	*slot = off;
	return parent;
}

static thmap_inode_t *
find_edge_node_locked(const thmap_t *thmap, thmap_query_t *query,
    const void * restrict key, size_t len, unsigned *slot)
{
	thmap_inode_t *node;
	thmap_ptr_t target;
retry:
	/*
	 * Find the edge node and lock it!  Re-check the state since
	 * the tree might change by the time we acquire the lock.
	 */
	node = find_edge_node(thmap, query, key, len, slot);
	lock_node(node);
	if (__predict_false(node->state & NODE_DELETED)) {
		/*
		 * The node has been deleted.  The tree might have a new
		 * shape now, therefore we must re-start from the root.
		 */
		unlock_node(node);
		query->level = 0;
		goto retry;
	}
	target = node->slots[*slot];
	if (__predict_false(target && THMAP_INODE_P(target))) {
		/*
		 * The target slot is has been changed and it is now
		 * an intermediate lock.  Re-start from the root.
		 */
		unlock_node(node);
		query->level = 0;
		goto retry;
	}
	return node;
}

/*
 * thmap_get: lookup a value given the key.
 */
void *
thmap_get(thmap_t *thmap, const void *key, size_t len)
{
	thmap_query_t query = THMAP_QUERY_INIT(0);
	thmap_inode_t *parent;
	thmap_leaf_t *leaf;
	unsigned slot;

	parent = find_edge_node(thmap, &query, key, len, &slot);
	leaf = get_leaf(thmap, parent, slot);
	if (!leaf) {
		return NULL;
	}
	if (!key_cmp_p(thmap, leaf, key, len)) {
		return NULL;
	}
	return leaf->val;
}

/*
 * thmap_put: insert a value given the key.
 *
 * => If the key is already present, return the associated value.
 * => Otherwise, on successful insert, return the given value.
 */
void *
thmap_put(thmap_t *thmap, const void *key, size_t len, void *val)
{
	thmap_query_t query = THMAP_QUERY_INIT(0);
	thmap_leaf_t *leaf, *other;
	thmap_inode_t *parent, *child;
	unsigned slot, other_slot;
	thmap_ptr_t target;

	/*
	 * First, pre-allocate and initialise the leaf node.
	 *
	 * NOTE: locking of the edge node below will issue the
	 * store fence for us.
	 */
	leaf = leaf_create(thmap, key, len, val);
	if (__predict_false(!leaf)) {
		return NULL;
	}

	/*
	 * Find the edge node and the target slot.
	 */
	parent = find_edge_node_locked(thmap, &query, key, len, &slot);
	target = parent->slots[slot]; // tagged offset
	if (THMAP_INODE_P(target)) {
		/*
		 * Empty slot: simply insert the new leaf.  The store
		 * fence is already issued for us.
		 */
		target = THMAP_GETOFF(thmap, leaf) | THMAP_LEAF_BIT;
		node_insert(parent, slot, target);
		goto out;
	}

	/*
	 * Collision or duplicate.
	 */
	other = THMAP_NODE(thmap, target);
	if (key_cmp_p(thmap, other, key, len)) {
		/*
		 * Duplicate.  Free the pre-allocated leaf and
		 * return the present value.
		 */
		val = leaf_free(thmap, leaf);
		goto out;
	}
descend:
	/*
	 * Collision -- expand the tree.  Create an intermediate node
	 * which will be locked (NODE_LOCKED) for us.  At this point,
	 * we advance to the next level.
	 */
	child = node_create(thmap, parent);
	if (__predict_false(!child)) {
		leaf_free(thmap, leaf);
		val = NULL;
		goto out;
	}
	query.level++;

	/*
	 * Insert the other (colliding) leaf first.
	 */
	other_slot = hashval_getleafslot(thmap, other, query.level);
	target = THMAP_GETOFF(thmap, other) | THMAP_LEAF_BIT;
	node_insert(child, other_slot, target);

	/*
	 * Insert the intermediate node into the parent node.
	 * It becomes the new parent for the our new leaf.
	 *
	 * Ensure that stores to the child (and leaf) reach the
	 * global visibility before it gets inserted to the parent.
	 */
	atomic_thread_fence(memory_order_stores);
	parent->slots[slot] = THMAP_GETOFF(thmap, child);

	unlock_node(parent);
	ASSERT(node_locked_p(child));
	parent = child;

	/*
	 * Get the new slot and check for another collision
	 * at the next level.
	 */
	slot = hashval_getslot(&query, key, len);
	if (slot == other_slot) {
		/* Another collision -- descend and expand again. */
		goto descend;
	}

	/* Insert our new leaf once we expanded enough. */
	target = THMAP_GETOFF(thmap, leaf) | THMAP_LEAF_BIT;
	node_insert(parent, slot, target);
out:
	unlock_node(parent);
	return val;
}

/*
 * thmap_del: remove the entry given the key.
 */
void *
thmap_del(thmap_t *thmap, const void *key, size_t len)
{
	thmap_query_t query = THMAP_QUERY_INIT(0);
	thmap_leaf_t *leaf;
	thmap_inode_t *parent;
	unsigned slot;
	void *val;

	parent = find_edge_node_locked(thmap, &query, key, len, &slot);
	leaf = get_leaf(thmap, parent, slot);
	if (!leaf || !key_cmp_p(thmap, leaf, key, len)) {
		/* Not found. */
		unlock_node(parent);
		return NULL;
	}

	/* Remove the leaf. */
	ASSERT(THMAP_NODE(thmap, parent->slots[slot]) == leaf);
	node_remove(parent, slot);

	/*
	 * Collapse the levels if removing the last item.
	 */
	while (query.level && NODE_COUNT(parent->state) == 0) {
		thmap_inode_t *node = parent;

		ASSERT(node->state == NODE_LOCKED);

		/*
		 * Ascend one level up.
		 * => Mark our current parent as deleted.
		 * => Lock the parent one level up.
		 */
		query.level--;
		slot = hashval_getslot(&query, key, len);
		parent = THMAP_NODE(thmap, node->parent);

		lock_node(parent);
		ASSERT((parent->state & NODE_DELETED) == 0);

		node->state |= NODE_DELETED;
		unlock_node(node); // memory_order_stores

		ASSERT(THMAP_NODE(thmap, parent->slots[slot]) == node);
		node_remove(parent, slot);

		/* Stage the removed node for G/C. */
		stage_mem_gc(thmap, THMAP_GETOFF(thmap, node), THMAP_INODE_LEN);
	}
	unlock_node(parent);

	/*
	 * Save the value and stage the leaf for G/C.
	 */
	val = leaf->val;
	if ((thmap->flags & THMAP_NOCOPY) == 0) {
		stage_mem_gc(thmap, leaf->key, leaf->len);
	}
	stage_mem_gc(thmap, THMAP_GETOFF(thmap, leaf), sizeof(thmap_leaf_t));
	return val;
}

/*
 * G/C routines.
 */

static void
stage_mem_gc(thmap_t *thmap, uintptr_t addr, size_t len)
{
	thmap_gc_t *head, *gc;

	gc = malloc(sizeof(thmap_gc_t));
	gc->addr = addr;
	gc->len = len;
retry:
	gc->next = head = thmap->gc_list;
	if (!atomic_compare_exchange_weak(&thmap->gc_list, head, gc)) {
		goto retry;
	}
}

void
thmap_gc(thmap_t *thmap)
{
	thmap_gc_t *gc;

	gc = atomic_exchange(&thmap->gc_list, NULL);
	while (gc) {
		thmap_gc_t *next = gc->next;
		thmap->ops->free(gc->addr, gc->len);
		free(gc);
		gc = next;
	}
}

/*
 * thmap_create: construct a new trie-hash map object.
 */
thmap_t *
thmap_create(uintptr_t baseptr, const thmap_ops_t *ops, unsigned flags)
{
	thmap_t *thmap;
	uintptr_t root;

	/*
	 * Setup the map object.
	 */
	if (!THMAP_ALIGNED_P(baseptr)) {
		return NULL;
	}
	thmap = calloc(1, sizeof(thmap_t));
	if (!thmap) {
		return NULL;
	}
	thmap->baseptr = baseptr;
	thmap->ops = ops ? ops : &thmap_default_ops;
	thmap->flags = flags;

	/* Allocate the root node. */
	root = thmap->ops->alloc(THMAP_ROOT_LEN);
	if (!root) {
		free(thmap);
		return NULL;
	}
	thmap->root = THMAP_GETPTR(thmap, root);
	memset(thmap->root, 0, THMAP_ROOT_LEN);
	return thmap;
}

void
thmap_destroy(thmap_t *thmap)
{
	uintptr_t root = THMAP_GETOFF(thmap, thmap->root);
	thmap->ops->free(root, THMAP_ROOT_LEN);
	free(thmap);
}
