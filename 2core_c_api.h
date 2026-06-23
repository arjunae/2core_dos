#ifndef TWOCORE_C_API_H
#define TWOCORE_C_API_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Ring-Buffer Slot-Strukturen -- identisch mit worker.cc
// ---------------------------------------------------------------------------
struct data1Slot {
    uint32_t ticketId;
    uint32_t physSrc;
    uint32_t physDst;
    uint32_t pxlWdth;
    uint32_t pxlHght;
} __attribute__((packed, aligned(4)));

struct data2Slot {
    uint32_t ticketId;
    uint32_t physSrc;
    uint32_t physDst;
    uint32_t smpCnt;
} __attribute__((packed, aligned(4)));

struct data3Slot {
    uint32_t ticketId;
    uint32_t physCoeffs;
    uint32_t physOutput;
    uint32_t physWorkspace;
    uint32_t physRevtab;
    uint32_t physTcos;
    uint32_t physTsin;
    uint32_t physFftTcos;
    uint32_t physFftTsin;
} __attribute__((packed, aligned(4)));

// ---------------------------------------------------------------------------
// Ring-Buffer Header-Strukturen
// ---------------------------------------------------------------------------
struct Data1Ring {
    volatile uint32_t headIdx;
    volatile uint32_t tailIdx;
    uint32_t          maxItem;
    struct data1Slot  dataArr[16];
} __attribute__((packed, aligned(4)));

struct Data2Ring {
    volatile uint32_t headIdx;
    volatile uint32_t tailIdx;
    uint32_t          maxItem;
    struct data2Slot  dataArr[32];
} __attribute__((packed, aligned(4)));

struct Data3Ring {
    volatile uint32_t headIdx;
    volatile uint32_t tailIdx;
    uint32_t          maxItem;
    struct data3Slot  dataArr[16];
} __attribute__((packed, aligned(4)));

// ---------------------------------------------------------------------------
// pipeDat -- alle drei Pipes, identisch mit worker.cc
// ---------------------------------------------------------------------------
struct pipeDat {
    struct Data1Ring ring_buff1;  // offset   0, size 332
    struct Data2Ring ring_buff2;  // offset 332, size 524
    struct Data3Ring ring_buff3;  // offset 856
} __attribute__((packed, aligned(4)));

// ---------------------------------------------------------------------------
// Core2 C-API
// ---------------------------------------------------------------------------
void     core2_shutdown(void);
void     core2_init(void);
uint32_t core2_prepTram(uint32_t dosBlockBase, uint32_t workerEntryPhys);
uint32_t core2_getTramPhys(void);       // Alias: gibt memoryBase zurueck
uint32_t core2_startAp(uint8_t apicId);
void     core2_spawn(uint32_t blockStart);
void     core2_syncHeadIdx(void);
void     core2_sendDataOffset(void *sourcePointer, uint32_t dataSize, uint32_t offset);
void     core2_sendData(void *sourcePointer, uint32_t dataSize);
void     core2_handlePendingInterrupts(void);
bool     core2_checkTaskEvent(uint32_t *outTicket, uint32_t *outReady, uint32_t *outMsg, char* outStr);
bool     core2_setupSys(const char* filePath);

uint32_t core2_getPayloadBase(void);
uint32_t core2_getDataOff(void);
uint32_t core2_allocPhys(uint32_t size);

void     core2_writeLinear(uint32_t linearAddr, void* src, uint32_t size);
void     core2_readLinear(uint32_t linearAddr, void* dst, uint32_t size);

// loadBin: laedt eine binaere Datei in physischen Speicher.
// Rueckgabe: linearer Zeiger (DOS-Segment), oder NULL bei Fehler.
// physAddr wird mit der physikalischen Basisadresse befuellt.
//void*    loadBin(const char* filePath, uint32_t* physAddr);
void*    core2_loadBin(const char* filePath, uint32_t* physAddr);

#ifdef __cplusplus
}
#endif

#endif
