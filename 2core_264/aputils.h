#pragma GCC optimize ("-fno-stack-protector")
#include "../2core.h"

// baremetal minimal mem and utility lib
// api adapted from https //musl.libc.org/  rmemcpy rmemove rmemset rstrlen 
// api copied from https //github.com/PetteriAimonen/libfixmath 
// fix16_add fix16_sub fix16_sadd fix16_ssub fix16_mul fix16_smul clz fix16_div fix16_sdiv fix16_mod fix16_lerp8 fix16_lerp16 fix16_lerp32
// all copyright is held by the above owners

struct dpmiReg {
	uint32_t regEax;
	uint32_t regEbx;
	uint32_t regEcx;
	uint32_t regEdx;
	uint32_t regEsi;
	uint32_t regEdi;
	uint32_t regFlg;
};

// Fix 1 inline barrier instead of wbinvd for cache coherence in SMP context
// wbinvd is a serializing privileged instruction that can trigger a VM exit or GP fault in VirtualBox if the DPMI host does not correctly delegate it to Ring 0 mfence and cpuid serialize memory reliably without this risk
#define MEM_BARRIER() asm volatile("mfence" ::: "memory")

// CPUID leaf 1 EDX bit 19 CLFLUSH available
#define DETECT_CPU_FEATURES(out_edx) \
	do { \
		asm volatile( \
			"mov $1, %%eax\n\t" \
			"cpuid\n\t" \
			: "=d"(out_edx) \
			: \
			: "eax", "ebx", "ecx" \
		); \
		(out_edx) = ((out_edx) >> 19) & 1; \
	} while(0)

// Fix 2 only execute clflush if CPUID feature bit is set VirtualBox usually emulates clflush correctly but it may be missing on very old versions or with certain CPU profiles we check once at startup and fall back to mfence
#define SAFE_CLFLUSH(addr, len) do { \
	uint32_t _start = (uint32_t)(addr) & ~63; \
	uint32_t _end = ((uint32_t)(addr) + (len) + 63) & ~63; \
	for (uint32_t _i = _start; _i < _end; _i += 64) { \
		asm volatile("clflush (%0)" :: "r"(_i) : "memory"); \
	} \
	asm volatile("mfence" ::: "memory"); \
} while(0)

static inline void* __attribute__((always_inline)) rmemset(void* destPointer, int inValue, uint32_t byteCount) {
	uint32_t destAddress = (uint32_t)destPointer;
	uint32_t wordCount = byteCount >> 2;
	uint32_t byteRemain = byteCount & 3;
	uint32_t fillValue = (inValue & 0xFF) * 0x01010101;
	
	__asm__ __volatile__ (
		"cld\n\t"
		"rep stosl\n\t"
		"mov %3, %%ecx\n\t"
		"rep stosb"
		: "+D" (destAddress), "+c" (wordCount)
		: "a" (fillValue), "r" (byteRemain)
		: "memory", "cc"
	);
	
	return destPointer;
}

#define CALL_INT(tskPtr, idInt, regPtr) \
	do { \
		(tskPtr)->intEax = (regPtr)->regEax; \
		(tskPtr)->intEbx = (regPtr)->regEbx; \
		(tskPtr)->intEcx = (regPtr)->regEcx; \
		(tskPtr)->intEdx = (regPtr)->regEdx; \
		(tskPtr)->intEsi = (regPtr)->regEsi; \
		(tskPtr)->intEdi = (regPtr)->regEdi; \
		(tskPtr)->intFlg = (regPtr)->regFlg; \
		(tskPtr)->numInt = (idInt); \
		MEM_BARRIER(); \
		(tskPtr)->reqInt = 1; \
		while ((tskPtr)->reqInt != 2 && (uint32_t)(tskPtr)->cmd != 2) { \
			asm volatile("pause"); \
		} \
		(regPtr)->regEax = (tskPtr)->intEax; \
		(regPtr)->regEbx = (tskPtr)->intEbx; \
		(regPtr)->regEcx = (tskPtr)->intEcx; \
		(regPtr)->regEdx = (tskPtr)->intEdx; \
		(regPtr)->regEsi = (tskPtr)->intEsi; \
		(regPtr)->regEdi = (tskPtr)->intEdi; \
		(regPtr)->regFlg = (tskPtr)->intFlg; \
		MEM_BARRIER(); \
		(tskPtr)->reqInt = 0; \
	} while(0)

// Cache flush for the ResultSlot so the main core sees it
#define QUEUE_RESULT(taskPtr, ticket, ready, msg, useFlush) \
	do { \
		uint32_t curHead = (taskPtr)->resRing.headIdx; \
		(taskPtr)->resRing.dataArr[curHead].ticketId = (ticket); \
		(taskPtr)->resRing.dataArr[curHead].readyFlag = (ready); \
		(taskPtr)->resRing.dataArr[curHead].msgVal = (msg); \
		SAFE_CLFLUSH(&(taskPtr)->resRing.dataArr[curHead], (useFlush)); \
		SAFE_CLFLUSH((char*)&(taskPtr)->resRing.dataArr[curHead] + 32, (useFlush)); \
		asm volatile("mfence" ::: "memory"); \
		(taskPtr)->resRing.headIdx = (curHead + 1) % (taskPtr)->resRing.maxItem; \
		SAFE_CLFLUSH(&(taskPtr)->resRing.headIdx, (useFlush)); \
	} while(0)

#define PRINT_VGA(col, c) \
	do { \
		volatile uint16_t* vga = (volatile uint16_t*)0xB8000; \
		vga[(col)] = 0x4F00 | (uint16_t)(c); \
	} while(0)

// das attribut verhindert dass der compeiler relative calls baut die nach dem kopieren ins leere springen
static inline void* __attribute__((always_inline)) rmemcpy(void* destPtr, const void* srcPtr, uint32_t byteCount) {
	uint32_t destAddr = (uint32_t)destPtr;
	uint32_t srcAddr = (uint32_t)srcPtr;
	uint32_t wordCount = byteCount >> 2;
	uint32_t byteRemain = byteCount & 3;
	
	// cld ist ueberlebenswichtig weil der nackte ring 0 start state des direction flags voellig undefiniert ist
	__asm__ __volatile__ (
		"cld\n\t"
		"rep movsl\n\t"
		"mov %3, %%ecx\n\t"
		"rep movsb"
		: "+D" (destAddr), "+S" (srcAddr), "+c" (wordCount)
		: "r" (byteRemain)
		: "memory", "cc"
	);
	
	return destPtr;
}

static inline void* __attribute__((always_inline)) rmemmove(void* destPtr, const void* srcPtr, uint32_t byteCount) {
	uint32_t destAddr = (uint32_t)destPtr;
	uint32_t srcAddr = (uint32_t)srcPtr;
	
	if (destAddr - srcAddr < byteCount) {
		destAddr += byteCount - 1;
		srcAddr += byteCount - 1;
		
		// std erzwingt rueckwaerts lauf bei overlap aber cld danach ist absolute pflicht fuer die gcc abi
		// outportb(0x80, 0xAA)
		
		__asm__ __volatile__ (
			"std\n\t"
			"rep movsb\n\t"
			"cld"
			: "+D" (destAddr), "+S" (srcAddr), "+c" (byteCount)
			:
			: "memory", "cc"
		);
	} else {
		uint32_t wordCount = byteCount >> 2;
		uint32_t byteRemain = byteCount & 3;
		
		__asm__ __volatile__ (
			"cld\n\t"
			"rep movsl\n\t"
			"mov %3, %%ecx\n\t"
			"rep movsb"
			: "+D" (destAddr), "+S" (srcAddr), "+c" (wordCount)
			: "r" (byteRemain)
			: "memory", "cc"
		);
	}
	
	return destPtr;
}

static inline uint32_t __attribute__((always_inline)) rstrlen(const char* strPtr) {
	uint32_t strLength;
	uint32_t dummyIndex;
	
	// ohne cld wuerde scasb blind rueckwaerts in den speicher rattern und sofort den stack zerschiessen
	__asm__ __volatile__ (
		"cld\n\t"
		"repne scasb\n\t"
		"not %0\n\t"
		"dec %0"
		: "=c" (strLength), "=D" (dummyIndex)
		: "1" (strPtr), "a" (0), "0" (0xFFFFFFFF)
		: "memory", "cc"
	);
	
	return strLength;
}

#define rSTR(strAdr) ((char*)((uint32_t)(strAdr) + (tskPtr->funcPtr)))

// copy msg to addr
#define DBG_MSG(destPtr, strMsg) \
	asm volatile( \
		"jmp 2f\n" \
		"1:\n" \
		"pop %%esi\n" \
		"mov %0, %%edi\n" \
		"3:\n" \
		"lodsb\n" \
		"stosb\n" \
		"test %%al, %%al\n" \
		"jnz 3b\n" \
		"jmp 4f\n" \
		"2:\n" \
		"call 1b\n" \
		".asciz \"" strMsg "\"\n" \
		"4:\n" \
		: \
		: "r" (destPtr) \
		: "esi", "edi", "eax", "memory" \
	)

// HILFSFUNKTION Integer zu ASCII Zielpuffer (Unterstuetzt Basis 10 und 16)
static inline int __attribute__((always_inline)) ritoa_buf(int32_t value, char* buf, int base) {
	char tmp[32]; 
	int i = 0;
	uint32_t uvalue = value;

	// Vorzeichen Handling nur bei Basis 10 noetig
	if (base == 10 && value < 0) {
		uvalue = -value;
	}

	// Ziffern von hinten nach vorne generieren
	do {
		int rem = uvalue % base;
		tmp[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
		uvalue /= base;
	} while (uvalue > 0);

	// Minuszeichen anhaengen
	if (base == 10 && value < 0) {
		tmp[i++] = '-';
	}

	// Da die Ziffern rueckwaerts generiert wurden jetzt umgedreht ins Ziel schreiben
	int len = i;
	while (i > 0) {
		*buf++ = tmp[--i];
	}
	return len; 
}

static inline void __attribute__((always_inline)) rprintf_impl(TaskInt* tsk, const char* fmt, __builtin_va_list args) {
	char* dst = (char*)tsk->debugString;
	int max_chars = 511;
	int written = 0;

	while (*fmt && written < max_chars) {
		if (*fmt == '%') {
			fmt++;
			if (*fmt == '\0') break;
			if (*fmt == 's') {
				char* s = __builtin_va_arg(args, char*);
				while (*s && written < max_chars) dst[written++] = *s++;
			} else if (*fmt == 'd') {
				int32_t d = __builtin_va_arg(args, int32_t);
				written += ritoa_buf(d, &dst[written], 10);
			} else if (*fmt == 'x') {
				uint32_t x = __builtin_va_arg(args, uint32_t);
				written += ritoa_buf(x, &dst[written], 16);
			} else if (*fmt == 'f') {
				double f = __builtin_va_arg(args, double);
				if (f < 0) { dst[written++] = '-'; f = -f; }
				int32_t int_part  = (int32_t)f;
				int32_t frac_part = (int32_t)((f - (double)int_part) * 10000.0 + 0.5);
				if (frac_part >= 10000) { int_part++; frac_part -= 10000; }
				written += ritoa_buf(int_part, &dst[written], 10);
				if (written < max_chars) dst[written++] = '.';
				char frac_str[6];
				int frac_len = ritoa_buf(frac_part, frac_str, 10);
				int leading_zeros = 4 - frac_len;
				while (leading_zeros-- > 0 && written < max_chars) dst[written++] = '0';
				for (int i = 0; i < frac_len && written < max_chars; i++) dst[written++] = frac_str[i];
			} else {
				dst[written++] = *fmt;
			}
		} else {
			dst[written++] = *fmt;
		}
		fmt++;
	}
	dst[written] = '\0';
}

static void rprintf(TaskInt* tsk, const char* fmt, ...) {
	__builtin_va_list args;
	__builtin_va_start(args, fmt);
	rprintf_impl(tsk, fmt, args);
	__builtin_va_end(args);
}

struct MemBlock {
	uint32_t size;
	int isFree;
	MemBlock* next;
};

extern MemBlock* freeList;

static inline void __attribute__((always_inline)) rheapInit(void* heapStart, uint32_t heapSize) {
	freeList = (MemBlock*)heapStart;
	freeList->size = heapSize - sizeof(MemBlock);
	freeList->isFree = 1;
	freeList->next = nullptr;
}

static inline void* __attribute__((always_inline)) rmalloc(uint32_t reqSize) {
	if (reqSize == 0) return nullptr;
	
	uint32_t alignSize = (reqSize + 7) & ~7;
	MemBlock* currBlock = freeList;
	
	while (currBlock) {
		if (currBlock->isFree && currBlock->size >= alignSize) {
			// split block if enough space remains
			if (currBlock->size >= alignSize + sizeof(MemBlock) + 8) {
				MemBlock* nextBlock = (MemBlock*)((uint8_t*)currBlock + sizeof(MemBlock) + alignSize);
				nextBlock->size = currBlock->size - alignSize - sizeof(MemBlock);
				nextBlock->isFree = 1;
				nextBlock->next = currBlock->next;
				
				currBlock->size = alignSize;
				currBlock->next = nextBlock;
			}
			currBlock->isFree = 0;
			return (void*)((uint8_t*)currBlock + sizeof(MemBlock));
		}
		currBlock = currBlock->next;
	}
	return nullptr;
}

static inline int __attribute__((always_inline)) rmemcmp(const void* a, const void* b, uint32_t n) {
	const unsigned char* p1 = (const unsigned char*)a;
	const unsigned char* p2 = (const unsigned char*)b;
	for (uint32_t i = 0; i < n; i++)
		if (p1[i] != p2[i]) return p1[i] - p2[i];
	return 0;
}

static inline char* __attribute__((always_inline)) rstrcpy(char* dest, const char* src) {
	char* d = dest;
	while ((*d++ = *src++));
	return dest;
}

static inline int __attribute__((always_inline)) rstrcmp(const char* s1, const char* s2) {
	while (*s1 && (*s1 == *s2)) { s1++; s2++; }
	return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static inline void __attribute__((always_inline)) rfree(void* ptr) {
	if (!ptr) return;
	
	MemBlock* currBlock = (MemBlock*)((uint8_t*)ptr - sizeof(MemBlock));
	currBlock->isFree = 1;
	
	// combine adjacent free blocks
	MemBlock* tempBlock = freeList;
	while (tempBlock) {
		if (tempBlock->isFree && tempBlock->next && tempBlock->next->isFree) {
			tempBlock->size += sizeof(MemBlock) + tempBlock->next->size;
			tempBlock->next = tempBlock->next->next;
		} else {
			tempBlock = tempBlock->next;
		}
	}
}

static inline uint32_t __attribute__((always_inline)) rheapGetFree() {
	uint32_t total = 0;
	MemBlock* currBlock = freeList;
	while (currBlock) {
		if (currBlock->isFree) total += currBlock->size;
		currBlock = currBlock->next;
	}
	return total;
}

static inline uint32_t __attribute__((always_inline)) rheapGetMaxFree() {
	uint32_t maxFree = 0;
	MemBlock* currBlock = freeList;
	while (currBlock) {
		if (currBlock->isFree && currBlock->size > maxFree) maxFree = currBlock->size;
		currBlock = currBlock->next;
	}
	return maxFree;
}


#ifndef APUTILS_NO_LIBC_ALIASES
// Freestanding runtime GCC may emit calls to these even in ffreestanding
// struct copies array init loop idiom recognition in the inlined imdct
// Forward to the asm primitives so GCC can not lower the bodies back into a
// memcpy memset call that would be infinite recursion
extern "C" {
inline void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n) {
	return rmemcpy(dst, src, (uint32_t)n);
}
inline void *memset(void *dst, int c, __SIZE_TYPE__ n) {
	return rmemset(dst, c, (uint32_t)n);
}
inline void *memmove(void *dst, const void *src, __SIZE_TYPE__ n) {
	return rmemmove(dst, src, (uint32_t)n);
}
inline int    memcmp(const void *a, const void *b, __SIZE_TYPE__ n) { return rmemcmp(a, b, (uint32_t)n); }
inline __SIZE_TYPE__ strlen(const char *s)                         { return rstrlen(s); }
inline char  *strcpy(char *d, const char *s)                       { return rstrcpy(d, s); }
inline int    strcmp(const char *a, const char *b)                 { return rstrcmp(a, b); }
}
#endif