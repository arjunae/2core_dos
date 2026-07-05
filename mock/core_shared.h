#ifndef CORE_SHARED_H
#define CORE_SHARED_H

#include <stdint.h>

// ABI contract between the producer BSP and the worker AP

// One job slot in the ring buffer.
struct work_slt {
	uint32_t val_in;   // input value supplied by the host
	uint32_t is_done;  // set to 1 by the worker when the slot is consumed
};

// Control block shared between host and worker, placed in shared memory.
struct share_mem {
	uint32_t head_idx;        // next slot the worker reads
	uint32_t tail_idx;        // next slot the host writes
	uint32_t max_item;        // ring capacity used for modulo is <= 32
	uint32_t heap_ptr;        // physical base of the shared result heap
	uint32_t heap_sze;        // size of the result heap in bytes
	struct work_slt slots[32];// the actual ring buffer storage
};

#endif