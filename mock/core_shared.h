#ifndef CORE_SHARED_H
#define CORE_SHARED_H

#include <stdint.h>

// ABI contract between the producer BSP and the worker AP

// The control block sits at dataPhysical + AddWkOff behind the task rings
#define AddWkOff       0x1000u
#define FadWkOff       0u

// ResultRing doorbell The worker posts this ticket when the batch is done
// the producer waits for exactly this ticket via core2 checkTaskEvent
#define AddTicket      0x0000CAFEu
#define FadTicket      0x0000FAADu
#define WORK_DONE_MSG  0x0000D09Eu

// cmd producer to worker
#define WCMD_IDLE      0u
#define WCMD_RUN       1u
#define WCMD_STOP      2u

// status worker to producer heartbeat the libs startAp waits for ==2
#define WST_BOOT       0u
#define WST_ALIVE      2u
#define WST_EXIT       3u

// Payload geometry NUM_COUNT 32 bit integers are processed in place
#define NUM_COUNT      16u
#define ADD_CONSTANT   15      // the worker adds this to every element

 
// One job slot in the ring buffer.
struct work_slt {
	uint32_t val_in;   // input value supplied by the host
	uint32_t is_done;  // set to 1 by the worker once the slot is consumed
};
 
// Control block shared between host and worker, placed in shared memory.
struct share_mem {
	uint32_t head_idx;        // next slot the worker reads   (worker-owned)
	uint32_t tail_idx;        // next slot the host writes     (host-owned)
	uint32_t max_item;        // ring capacity used for modulo (<= 32)
	uint32_t heap_ptr;        // physical base of the shared result heap
	uint32_t heap_sze;        // size of the result heap in bytes
	struct work_slt slots[32];// the actual ring buffer storage
};

// WorkBlock the entire cross core interface for this job
// cmd status doneCount apicId the bring up and command handshake
// Keep this packed so the byte layout is identical regardless of compiler padding rules on either side
struct WorkBlock {
	volatile uint32_t cmd;               // WCMD_*
	volatile uint32_t status;            // WST_* AP is alive
	volatile uint32_t doneCount;         // AP increments after each batch
	volatile uint32_t apicId;            // AP reports its LAPIC id

	volatile int32_t  values[NUM_COUNT]; // in place input then input+15
} __attribute__((packed, aligned(4)));

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

// Reserve fuer EINEN dekodierten AAC Frame Backpressure Headroom des Producers
// LC max 1024 Samples Kanal HE AAC SBR verdoppelt auf 2048 Stereo 16 bit
// 2048 * 2ch * 2B = 8192 Mit Sicherheitsmarge
#define PCM_MAX_FRAME_BYTES   16384u

// Pacing Der AP haelt nur diesen Vorlauf an PCM und idlet dann hlt statt den
// ganzen Ring vollzudekodieren Verhindert den CPU Burst am Anfang Underruns auf
// Maschinen mit wenigen Host Cores und haelt die Startverzoegerung kurz
#define PCM_LOW_WATER         (256u * 1024u)   // ~1.5s @ 44k 16 2

// Default Ringgroesse BSP setzt sd pcmRingBytes hier nur als Referenz
#define PCM_RING_BYTES_DEFAULT (4u * 1024u * 1024u)

// Streaming FaadData Steuer Handshake Felder und PCM Ring Zeiger
// Liegt wie frueher WorkBlock im DOS Konventionalpuffer die PCM Ringdaten
// selbst liegen separat im Highmem bei physPcm
struct FaadData {
	// Steuer Handshake
	volatile uint32_t cmd;           // WCMD_*
	volatile uint32_t status;        // WST_*
	volatile uint32_t apicId;
	volatile uint32_t formatReady;   // 1 = outSamplerate outChannels gueltig
	volatile uint32_t endOfStream;   // 1 = AP hat alle Frames dekodiert
	volatile uint32_t doneCount;     // Completion Zaehler steigt am Stream Ende
	volatile uint32_t outSamples;    // gesamt dekodierte Samples informativ
	volatile uint32_t outError;      // FAAD Fehlercode 0 = ok

	// Eingang AAC
	volatile uint32_t physSrc;
	volatile uint32_t srcLen;
	volatile uint32_t physHeap;
	volatile uint32_t heapSize;

	// Ausgabeformat gueltig sobald formatReady==1
	volatile uint32_t outSamplerate;
	volatile uint32_t outChannels;

	// PCM Ring Daten im Highmem bei physPcm
	volatile uint32_t physPcm;       // Basis des Ringpuffers Highmem
	volatile uint32_t pcmRingBytes;  // Ringgroesse in Bytes
	volatile uint32_t pcmWriteBytes; // absolut geschriebene Bytes NUR AP schreibt
	volatile uint32_t pcmReadBytes;  // absolut gelesene Bytes NUR BSP schreibt
} __attribute__((packed, aligned(4)));

#endif
