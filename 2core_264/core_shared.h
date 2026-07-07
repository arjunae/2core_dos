#ifndef CORE_SHARED_H
#define CORE_SHARED_H

#include <stdint.h>

// ABI contract between the producer BSP and the worker AP

#define MAX_NAL_SLOTS 32

// Structure representing a single NAL job in the ring buffer
struct NalSlot {
	volatile uint32_t ptr_phys; // Physical pointer to NAL data
	volatile uint32_t size;     // Size of the NAL unit
	volatile uint32_t result;   // Result code e g 0xDEADBEEF
	volatile uint32_t done;     // 1 if AP is finished 0 if pending
} __attribute__((packed, aligned(16)));

struct FrameInfo {
	uint32_t physY, physU, physV;
	uint32_t width, height;
	uint32_t strideY, strideC;
} __attribute__((packed, aligned(4)));

// Main shared memory block mapped at dataOff dataPhys
struct EdgeData {
	// Ring buffer state
	volatile uint32_t headIdx;
	volatile uint32_t tailIdx;
	volatile uint32_t maxItem;
	
	// CPU2 Heap configuration
	volatile uint32_t heapPtr;
	volatile uint32_t heapSize;
	
	// Array of NAL slots Starts exactly at offset 20
	struct NalSlot slots[MAX_NAL_SLOTS];
	
	// Padding to push VESA parameters safely out of the way
	// slots array is 32 * 16 = 512 bytes Offset is 20 + 512 = 532
	// VESA Parameters Offset 532
	volatile uint32_t vesa_lfb;
	volatile uint32_t back_buffer;
	volatile int32_t  vesa_pitch;
	volatile int32_t  vesa_bpp;
	volatile int32_t  vesa_width;
	volatile int32_t  vesa_height;
	
	// Padding to push debug variables further out to offset 1000
	// Current offset 532 + 24 = 556 1000 minus 556 = 444 bytes of padding needed
	uint8_t _pad2[444];
	
	// AP Debug Variables Offset 1000
	volatile uint32_t ap_debug;
	
} __attribute__((packed, aligned(4)));

#endif
