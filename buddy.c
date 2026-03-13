#include "buddy.h"
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)
#define MAX_PAGES 65536

// Free list structure - stores page indices
typedef struct list_node {
    int page_idx;
    struct list_node *next;
} list_node_t;

// Global state
static void *memory_base = NULL;
static int total_pages = 0;
static list_node_t *free_lists[MAX_RANK + 1];
static unsigned char page_states[MAX_PAGES]; // 0=free, 1=allocated
static unsigned char page_ranks[MAX_PAGES];   // rank of block starting at this page

// Storage for list nodes with recycling
static list_node_t node_pool[MAX_PAGES * 2];
static list_node_t *free_node_list = NULL;
static int node_pool_idx = 0;

// Helper: Get page index from address
static inline int addr_to_page_idx(void *addr) {
    if (addr < memory_base) return -1;
    long offset = (char*)addr - (char*)memory_base;
    if (offset < 0 || offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

// Helper: Get address from page index
static inline  void *page_idx_to_addr(int idx) {
    return (char*)memory_base + (long)idx * PAGE_SIZE;
}

// Helper: Get buddy index
static inline int get_buddy_idx(int page_idx, int rank) {
    int block_size = (1 << (rank - 1));
    return page_idx ^ block_size;
}

// Helper: Allocate a node from pool or free list
static list_node_t *alloc_node() {
    if (free_node_list != NULL) {
        list_node_t *node = free_node_list;
        free_node_list = node->next;
        return node;
    }
    if (node_pool_idx >= MAX_PAGES * 2) return NULL;
    return &node_pool[node_pool_idx++];
}

// Helper: Free a node back to free list
static void free_node(list_node_t *node) {
    node->next = free_node_list;
    free_node_list = node;
}

// Helper: Find and remove from free list
static int remove_from_free_list(int rank, int page_idx) {
    list_node_t **curr = &free_lists[rank];

    while (*curr != NULL) {
        if ((*curr)->page_idx == page_idx) {
            list_node_t *to_free = *curr;
            *curr = (*curr)->next;
            free_node(to_free);
            return 1;
        }
        curr = &((*curr)->next);
    }
    return 0;
}

// Helper: Add to free list
static void add_to_free_list(int rank, int page_idx) {
    list_node_t *node = alloc_node();
    if (!node) return;
    node->page_idx = page_idx;
    node->next = free_lists[rank];
    free_lists[rank] = node;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) return -EINVAL;

    memory_base = p;
    total_pages = pgcount;
    node_pool_idx = 0;
    free_node_list = NULL;

    // Initialize free lists
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize page metadata
    for (int i = 0; i < MAX_PAGES; i++) {
        page_states[i] = 0;  // Free
        page_ranks[i] = 0;   // Unknown rank
    }

    // Add memory blocks to free lists, using largest possible blocks
    int idx = 0;
    while (idx < pgcount) {
        int remaining = pgcount - idx;
        int rank = MAX_RANK;

        // Find largest rank that fits
        while (rank > 0 && (1 << (rank - 1)) > remaining) {
            rank--;
        }

        // Also ensure block is aligned to its size
        while (rank > 0 && (idx % (1 << (rank - 1))) != 0) {
            rank--;
        }

        if (rank == 0) break;

        add_to_free_list(rank, idx);
        page_ranks[idx] = rank;

        idx += (1 << (rank - 1));
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find a free block of required rank or larger
    int current_rank = rank;
    while (current_rank <= MAX_RANK && free_lists[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Remove block from free list
    list_node_t *node = free_lists[current_rank];
    int page_idx = node->page_idx;
    free_lists[current_rank] = node->next;
    free_node(node);

    // Split block down to required rank
    while (current_rank > rank) {
        current_rank--;
        int buddy_idx = page_idx + (1 << (current_rank - 1));
        add_to_free_list(current_rank, buddy_idx);
        page_ranks[buddy_idx] = current_rank;
    }

    // Mark pages as allocated
    int block_size = (1 << (rank - 1));
    for (int i = 0; i < block_size; i++) {
        page_states[page_idx + i] = 1;
    }
    page_ranks[page_idx] = rank;

    return page_idx_to_addr(page_idx);
}

int return_pages(void *p) {
    if (p == NULL) return -EINVAL;

    int page_idx = addr_to_page_idx(p);
    if (page_idx < 0 || page_idx >= total_pages) {
        return -EINVAL;
    }

    if (page_states[page_idx] == 0) {
        return -EINVAL;  // Already free
    }

    int rank = page_ranks[page_idx];
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    // Mark pages as free
    int block_size = (1 << (rank - 1));
    for (int i = 0; i < block_size; i++) {
        page_states[page_idx + i] = 0;
    }

    // Try to merge with buddy iteratively
    while (rank < MAX_RANK) {
        int buddy_idx = get_buddy_idx(page_idx, rank);

        // Check if buddy is valid
        if (buddy_idx < 0 || buddy_idx >= total_pages) break;

        // Check if buddy is free
        if (page_states[buddy_idx] != 0) break;

        // Check if buddy has the same rank
        if (page_ranks[buddy_idx] != rank) break;

        // Remove buddy from its free list
        if (!remove_from_free_list(rank, buddy_idx)) break;

        // Merge: the merged block starts at the lower index
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
        }

        // Increase rank for merged block
        rank++;
        page_ranks[page_idx] = rank;
    }

    // Add the final merged block to its free list
    add_to_free_list(rank, page_idx);
    page_ranks[page_idx] = rank;

    return OK;
}

int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;

    int page_idx = addr_to_page_idx(p);
    if (page_idx < 0 || page_idx >= total_pages) {
        return -EINVAL;
    }

    return page_ranks[page_idx];
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    int count = 0;
    list_node_t *node = free_lists[rank];
    while (node != NULL) {
        count++;
        node = node->next;
    }

    return count;
}
