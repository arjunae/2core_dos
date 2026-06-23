// ---------------------------------------------------------------------------
// bareBones DOS multi core setup original by bloodwych around 2013 Foldername LIC BSD3CLAUSE
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <pc.h>
#include "2core.h"
#include <sys/nearptr.h>
#include <cstring>
// VGA-Textpuffer (Segment B800:0000, Farbe 0x07=grau auf schwarz, 0x20=Leerzeichen)
static constexpr uint32_t VGA_TEXT_BASE        = 0xB8000u;
static constexpr uint16_t VGA_BLANK_CELL       = 0x0720u;  // Attribut 07 + ' '

// Local APIC (Memory-Mapped I/O, physische Basisadresse)
static constexpr uint32_t LAPIC_PHYS_BASE      = 0xFEE00000u;

// Local APIC Register-Offsets (relativ zu LAPIC_PHYS_BASE)
static constexpr uint32_t LAPIC_SVR            = 0x0F0u;   // Spurious Vector Register
static constexpr uint32_t LAPIC_ICR_HIGH       = 0x310u;   // Interrupt Command Register (obere 32 Bit)
static constexpr uint32_t LAPIC_ICR_LOW        = 0x300u;   // Interrupt Command Register (untere 32 Bit)

// APIC SVR-Wert: APIC enable (Bit 8) + Spurious-Vektor 0xFF
static constexpr uint32_t LAPIC_SVR_ENABLE     = 0x1F0u;

// IPI-Kommandowörter für ICR_LOW
static constexpr uint32_t LAPIC_INIT_ASSERT    = 0x0000C500u;  // INIT IPI, Level=assert
static constexpr uint32_t LAPIC_INIT_DEASSERT  = 0x00008500u;  // INIT IPI, Level=deassert
static constexpr uint32_t LAPIC_SIPI_BASE      = 0x00004600u;  // STARTUP IPI (Vektor-Feld wird OR-verknüpft)
static constexpr uint32_t LAPIC_NMI_IPI        = 0x00004400u;  // NMI IPI (für AP-Wake)
static constexpr uint32_t LAPIC_INIT_NMI_STOP  = 0x000C4500u;  // INIT via NMI für erzwungenen AP-Stop

// ICR Delivery-Status
static constexpr uint32_t LAPIC_ICR_SEND_PENDING = (1u << 12);

// BIOS-Tick-Zähler ~18,2 Hz
static constexpr uint32_t BIOS_TICK_OFFSET     = 0x46Cu;

// Port für µs-Delays
static constexpr uint16_t PORT_DIAG            = 0x80u;

// AP-Panik-Marker, den der Exception-Handler in TaskInt::status schreibt
static constexpr uint32_t AP_PANIC_MARKER      = 0xEEu;

// Speicher-Layout relativ zu alignPhysical / dosBlockBase
static constexpr uint32_t MEM_OFF_TASK         = 0x2000u;  // TaskInt-Struktur
static constexpr uint32_t MEM_OFF_PAYLOAD      = 0x3000u;  // Payload-Puffer
static constexpr uint32_t MEM_OFF_DATA         = 0x7000u;  // Datenpuffer

// IDT-Offset innerhalb des Trampolins
static constexpr uint32_t TRAM_IDT_OFFSET      = 5120u;

// IDT-Gate-Felder
static constexpr uint16_t IDT_CS_SELECTOR      = 0x0018u;  // Flat 32-Bit-Code-Segment
static constexpr uint8_t  IDT_GATE_FLAGS       = 0x8Eu;    // Present, DPL=0

// Kapselt alle MMIO-Zugriffe auf den Local APIC
class LocalApic {
public:
	// Mappt LAPIC_PHYS_BASE in den Adressraum.  Gibt false zurück wenn
	// __dpmi_physical_address_mapping fehlschlägt (kein APIC vorhanden /
	// DPMI-Fehler).  Muss vor allen anderen Methoden aufgerufen werden.
	bool init() {
		sel_ = mapMem(LAPIC_PHYS_BASE);
		return sel_ != -1;
	}

	// Aktiviert den APIC und setzt den Spurious-Interrupt-Vektor.
	void enable() const {
		write(LAPIC_SVR, LAPIC_SVR_ENABLE);
	}

	// Sendet INIT IPI (assert) an die angegebene APIC-ID.
	void sendInitAssert(uint8_t apicId) const {
		writeIcr(apicId, LAPIC_INIT_ASSERT);
	}

	// Sendet INIT IPI (deassert) -- schließt die INIT-Sequenz ab.
	void sendInitDeassert(uint8_t apicId) const {
		writeIcr(apicId, LAPIC_INIT_DEASSERT);
	}

	// Sendet einen STARTUP IPI mit dem angegebenen Trampolin-Page-Index.
	void sendSipi(uint8_t apicId, uint8_t tramPage) const {
		writeIcr(apicId, LAPIC_SIPI_BASE | tramPage);
	}

	// Sendet einen NMI IPI -- weckt einen haltenden AP.
	void sendNmi(uint8_t apicId) const {
		writeIcr(apicId, LAPIC_NMI_IPI);
	}

	// Sendet INIT-via-NMI um den AP hart zu stoppen (erzwungener Shutdown).
	void sendInitNmiStop() const {
		write(LAPIC_ICR_LOW, LAPIC_INIT_NMI_STOP);
	}

	// Blockiert bis das Delivery-Status-Bit im ICR gelöscht ist (IPI zugestellt).
	// Verhindert verlorene IPIs in VirtualBox.
	void waitIdle() const {
		for (uint32_t spin = 0; spin < 1000000; spin++) {
			if ((read(LAPIC_ICR_LOW) & LAPIC_ICR_SEND_PENDING) == 0) return;
			asm volatile("pause");
		}
	}

	int sel() const { return sel_; }

private:
	int sel_ = -1;

	uint32_t read(uint32_t reg) const {
		return _farpeekl(sel_, reg);
	}

	void write(uint32_t reg, uint32_t val) const {
		_farpokel(sel_, reg, val);
	}

	// Schreibt zuerst ICR_HIGH (Ziel-APIC-ID), dann ICR_LOW (Befehl).
	// Die Reihenfolge ist laut Intel SDM zwingend: ICR_LOW löst den IPI aus.
	void writeIcr(uint8_t apicId, uint32_t cmd) const {
		write(LAPIC_ICR_HIGH, (uint32_t)apicId << 24);
		write(LAPIC_ICR_LOW,  cmd);
	}
};


extern "C" {
	extern uint8_t apStart[];
	extern uint8_t apEnd[];
	extern uint8_t apIdtIsrStubs[];
	extern uint8_t patchMain[];
	extern uint8_t patchArg1[];
	extern uint8_t patchArg2[];
	extern uint8_t patchStack[];
}

uint32_t g_workerStackOffset = 0;

CoreTwoAPI core2;
CoreTwoAPI *myPipe = &core2;
CoreTwoAPI *globTask = NULL;
void (*oldInt)(int) = NULL;
void (*oldTerm)(int) = NULL;
static uint32_t xms_entry = 0;
static uint8_t sysState = 0;

// Fix 1 echte APIC ID des AP merken wird in startAp gesetzt Auf VBox ist das 1
// auf echter HW haengt sie von der Topologie ab.
static uint8_t g_apId = 1;

// Singleton-Instanz des Local APIC -- wird in setupSys initialisiert.
static LocalApic apic_;
static void initXms() {
	__dpmi_regs regs;
	memset(&regs, 0, sizeof(regs));
	regs.x.ax = 0x4300;
	__dpmi_int(0x2F, &regs);
	if (regs.h.al != 0x80) {
		xms_entry = 0;
		return;
	}
	memset(&regs, 0, sizeof(regs));
	regs.x.ax = 0x4310;
	__dpmi_int(0x2F, &regs);
	xms_entry = (regs.x.es << 16) | regs.x.bx;
}

struct XMS_MoveStruct {
	uint32_t length;
	uint16_t srcHandle;
	uint32_t srcOffset;
	uint16_t destHandle;
	uint32_t destOffset;
} __attribute__((packed));

extern "C" {

void* loadBin(const char* filePath, uint32_t* physAddr) {
	if (xms_entry == 0) {
		initXms();
		if (xms_entry == 0) {
			printf("[DBG loadBin] FEHLER XMS-Treiber nicht gefunden\n");
			*physAddr = 0;
			return NULL;
		}
	}

	FILE* fp = fopen(filePath, "rb");
	if (!fp) {
		printf("[DBG loadBin] FEHLER kann %s nicht oeffnen\n", filePath);
		fflush(stdout);
		*physAddr = 0;
		return NULL;
	}

	fseek(fp, 0, SEEK_END);
	uint32_t byteSize = ftell(fp);
	rewind(fp);

	if (byteSize == 0) {
		printf("[DBG loadBin] FEHLER Datei ist leer\n");
		fclose(fp);
		*physAddr = 0;
		return NULL;
	}

	uint32_t stackSize = 150 * 1024; // 150KB total for AP stack, dataOff, taskOff, and transfer buffer
	g_workerStackOffset = byteSize + stackSize - 4; // Top of the allocation

	// DOS-Transferspeicher: 4096-Byte-Puffer + XMS-MoveStruct (258 Paragraphs)
	int dosSel = 0;
	int dosSeg = __dpmi_allocate_dos_memory(258, &dosSel);
	if (dosSeg == -1) {
		printf("[DBG loadBin] FEHLER Kein DOS-Transfer-Speicher frei\n");
		fclose(fp);
		*physAddr = 0;
		return NULL;
	}

	// Buffer beginnt bei dosSeg:0000
	uint8_t* dosLinearPtr = (uint8_t*)((dosSeg * 16) + __djgpp_conventional_base);

	// Struktur beginnt exakt 4096 Bytes (256 Paragraphs) spaeter
	uint32_t structSeg = dosSeg + 256;
	XMS_MoveStruct* moveInfoPtr = (XMS_MoveStruct*)((structSeg * 16) + __djgpp_conventional_base);

	// XMS High-Memory anfordern + sperren via allocXMS; Handle fuer den Move merken
	uint16_t xmsHandle = 0;
	uint32_t actPhys = allocXMS(byteSize + stackSize, &xmsHandle);
	if (actPhys == 0) {
		printf("[DBG loadBin] FEHLER XMS Allokation/Lock fehlgeschlagen\n");
		__dpmi_free_dos_memory(dosSel);
		fclose(fp);
		*physAddr = 0;
		return NULL;
	}

	// Struktur im DOS-Speicher initialisieren
	// WICHTIG: XMS erwartet bei Handle 0 ein Segment:Offset Format!
	__dpmi_regs rregs;
	moveInfoPtr->srcHandle  = 0;
	moveInfoPtr->srcOffset  = ((uint32_t)dosSeg << 16) | 0x0000;
	moveInfoPtr->destHandle = xmsHandle;

	uint32_t bytesLeft = byteSize;
	uint32_t currentDestOffset = 0;

	while (bytesLeft > 0) {
		uint32_t chunkSize = (bytesLeft > 4096) ? 4096 : bytesLeft;

		fread(dosLinearPtr, 1, chunkSize, fp);

		moveInfoPtr->length     = (chunkSize + 1) & ~1;
		moveInfoPtr->destOffset = currentDestOffset;

		// Saubere Uebergabe der Struktur via DS:SI an den XMS-Treiber
		memset(&rregs, 0, sizeof(rregs));
		rregs.x.ax = 0x0B00;
		rregs.x.ds = structSeg; // Segment der Struktur
		rregs.x.si = 0x0000;    // Offset 0
		rregs.x.cs = xms_entry >> 16;
		rregs.x.ip = xms_entry & 0xFFFF;

		__dpmi_simulate_real_mode_procedure_retf(&rregs);

		if (rregs.x.ax != 1) {
			printf("[DBG loadBin] FEHLER Sicherer XMS-Transfer fehlgeschlagen\n");
			__dpmi_free_dos_memory(dosSel);
			fclose(fp);
			*physAddr = 0;
			return NULL;
		}

		bytesLeft -= chunkSize;
		currentDestOffset += chunkSize;
	}

	__dpmi_free_dos_memory(dosSel);
	fclose(fp);

	*physAddr = actPhys;

	printf("[DBG loadBin] %s geladen via himem %lu bytes -> Phys=0x%08lX\n",
		   filePath, (unsigned long)byteSize, (unsigned long)*physAddr);
	fflush(stdout);

	sysState |= 1;
	return (void*)actPhys;
}

}

/* BSP seitiges clflush einer dos ds relativen Adresse Offset in Konventional
* speicher Erzwingt dass der BSP beim naechsten Read die vom AP geflushte
* Speicherzeile aus dem RAM neu laedt statt seine eigene veraltete Cache Kopie
* zu sehen Gleiche fs dos ds Technik wie in prepTram kein nearptr noetig
* Auf echter HW noetig in VBox SMP kohaerent nur ein No Op Kosten
*/
static inline void bspClflush(uint32_t dsOffset) {
	asm volatile(
		"push %%fs\n\t"
		"mov %1, %%fs\n\t"
		"clflush %%fs:(%0)\n\t"
		"pop %%fs"
		: : "r"(dsOffset), "rm"((unsigned short)_dos_ds) : "memory");
	asm volatile("mfence" ::: "memory");
}

/* AP-Panik-Report aus TaskInt lesen
* Red-Screen-Erkennung. Der AP-Exception-Handler (apboot.asm) schreibt im
* Fehlerfall in TaskInt (Offsets aus 2core.h):
*   status = 0xEE        Panik-Marker
*   intFlg = Vektor      (0=#DE, 6=#UD, 13=#GP, 14=#PF, ...) -- bewusst intFlg
*                        und NICHT intEax: intEax wird BSP-seitig (spawn,
*                        handlePendingInterrupts) beschrieben, dann ueberdeckt
*                        die dirty BSP-Kopie beim clflush den AP-Wert. intFlg
*                        fasst der BSP nirgends an -> kommt sauber durch.
*   intEcx = Error-Code  (CPU-Fehlercode, 0 falls keiner)
*   intEdx = EIP         (faulting instruction)
*   intEsi = CR2         (lineare Fehleradresse, nur bei #PF sinnvoll)
*   intEdi = EFLAGS
*   intEbx bleibt die Proof-of-Life-APIC-ID (alive check).
* Der Handler benutzt nur mfence (kein clflush), daher hier vor dem Lesen die
* betreffenden Cache-Zeilen invalidieren. Rueckgabe true bei vorliegender Panik;
* die Felder werden dann nach panic.txt geschrieben und auf stdout ausgegeben.
*/
static bool apPanicReport(TaskInt* taskPtr, uint32_t statusOff, uint32_t taskOff) {
	bspClflush(statusOff);
	if ((uint32_t)taskPtr->status != AP_PANIC_MARKER) return false;

	// intEax(548)..intFlg(572) liegen alle in derselben 64-Byte-Zeile (taskOff
	// ist seitenausgerichtet) -> ein einziger Flush deckt alle int-Felder ab.
	bspClflush(taskOff + 548);

	uint32_t vec = taskPtr->intFlg;          // Vektor (in intFlg, s.u.)
	uint32_t err = taskPtr->intEcx;          // Error-Code
	uint32_t eip = taskPtr->intEdx;          // faulting EIP
	uint32_t cr2 = taskPtr->intEsi;          // CR2
	uint32_t efl = taskPtr->intEdi;          // EFLAGS
	uint32_t aid = taskPtr->intEbx;          // Proof-of-Life APIC-ID

	FILE *f = fopen("panic.txt", "w");
	if (f) {
		fprintf(f, "AP PANIC EXC=%02lX ERR=%08lX EIP=%08lX CR2=%08lX EFLAGS=%08lX APICID=%lu\n",
		        (unsigned long)vec, (unsigned long)err, (unsigned long)eip,
		        (unsigned long)cr2, (unsigned long)efl, (unsigned long)aid);
		fclose(f);
	}
	printf("\n\n*** CORE 2 PANICKED! ***\n"
	       "  EXC=%02lX  ERR=%08lX  EIP=%08lX  CR2=%08lX  EFLAGS=%08lX  (APICID=%lu)\n\n",
	       (unsigned long)vec, (unsigned long)err, (unsigned long)eip,
	       (unsigned long)cr2, (unsigned long)efl, (unsigned long)aid);
	fflush(stdout);
	return true;
}
void CoreTwoAPI::handlePendingInterrupts() {
	asm volatile("mfence" ::: "memory");
	uint32_t intReq = taskPtr->reqInt;

	if (intReq == 1) {
		uint32_t runInt = taskPtr->numInt;

		__dpmi_regs rmRegs = {0};
		rmRegs.d.eax = taskPtr->intEax;
		rmRegs.d.ebx = taskPtr->intEbx;
		rmRegs.d.ecx = taskPtr->intEcx;
		rmRegs.d.edx = taskPtr->intEdx;

		__dpmi_int(runInt, &rmRegs);

		taskPtr->intEax = rmRegs.d.eax;
		taskPtr->intEbx = rmRegs.d.ebx;
		taskPtr->intEcx = rmRegs.d.ecx;
		taskPtr->intEdx = rmRegs.d.edx;

		asm volatile("mfence" ::: "memory");
		taskPtr->reqInt = 2;

		while (taskPtr->reqInt != 0) {
			asm volatile("pause");
		}
	}
}

bool CoreTwoAPI::checkTaskEvent(uint32_t &outTicket, uint32_t &outReady, uint32_t &outMsg, char* outStr) {
	static uint32_t pollCount = 0;
	bool doFlush = (pollCount++ % 100 == 0);

	if (doFlush) {
		if (apPanicReport(taskPtr, statusOff, taskOff)) {
			exit(1);
		}
	}
	asm volatile("" ::: "memory");

	uint32_t resRingO = taskOff + 584;
	
	// Echte HW headIdx wird vom AP geschrieben vor dem Read flushen
	// Sonst sieht der BSP ewig seine eigene veraltete Kopie
	if (doFlush) {
		bspClflush(resRingO + 0);
	}
	asm volatile("" ::: "memory");

	uint32_t head = taskPtr->resRing.headIdx;
	uint32_t tail = taskPtr->resRing.tailIdx;

	if (head != tail) {
		uint32_t dataArrPhys = taskPtr->resRing.dataArrPhys;
		uint32_t slotO = dataArrPhys + (tail * 76);

		// Slot Daten ebenfalls frisch aus dem RAM holen AP Schreibung
		bspClflush(slotO + 0);
		// Restliche Bytes des 76 Byte Slots flushen
		bspClflush(slotO + 64);

		outTicket = _farpeekl(_dos_ds, slotO + 0);
		outReady  = _farpeekl(_dos_ds, slotO + 4);
		outMsg    = _farpeekl(_dos_ds, slotO + 8);

		// Read string conditionally to avoid 64 byte reads
		if (outStr) {
			dosmemget(slotO + 12, 64, outStr);
			outStr[63] = '\0';
		}

		uint32_t maxItem = taskPtr->resRing.maxItem;
		uint32_t nextTail = (tail + 1) % maxItem;

		asm volatile("sfence" ::: "memory");
		taskPtr->resRing.tailIdx = nextTail;

		return true;
	}
	return false;
}

uint32_t logPhys(void *logPtr) {
	uint32_t linBase;
	__dpmi_get_segment_base_address(_my_ds(), &linBase);
	return linBase + (uint32_t)logPtr;
}

int mapMem(uint32_t physAddress) {
	__dpmi_meminfo memInfo;
	memInfo.address = physAddress;
	memInfo.size = 4096;
	if (__dpmi_physical_address_mapping(&memInfo) != 0) {
		return -1;
	}

	int localSel = __dpmi_allocate_ldt_descriptors(1);
	__dpmi_set_segment_base_address(localSel, memInfo.address);
	__dpmi_set_segment_limit(localSel, 4095);
	return localSel;
}

void microWait(uint32_t waitUs) {
	for (uint32_t idx = 0; idx < waitUs; idx++) {
		inportb(PORT_DIAG);
	}
}

void milliWait(uint32_t waitMs) {
	microWait(waitMs * 1000);
}

void endTask() {
	if (globTask != NULL) {
		// Clean AP stop using INIT assert and deassert to guarantee wait for SIPI state
		apic_.sendInitAssert(g_apId);
		milliWait(1);
		apic_.sendInitDeassert(g_apId);
	}
}


bool CoreTwoAPI::setupSys(const char* filePath) {
	globTask = this;
	alignPhysical = initMem();
	if (!apic_.init()) {
		printf("[setupSys] FEHLER Local APIC konnte nicht gemappt werden\n");
		fflush(stdout);
		return false;
	}

	// Phase 2 Worker Binary laden
	void* binPtr = loadBin(filePath, &this->workerPhys);
	if (!binPtr) {
		return false;
	}

	memoryBase  = alignPhysical;
	physAddress = alignPhysical;
	payloadBase = alignPhysical + MEM_OFF_PAYLOAD;
	taskOff     = alignPhysical + MEM_OFF_TASK;
	dataOff     = alignPhysical + MEM_OFF_DATA;

	// cbase Zwischenspeicher entfernt um den aktualisierten Wert zu nutzen
	taskPtr = (TaskInt*)(__djgpp_conventional_base + taskOff);

	commandOff  = taskOff + 0;
	statusOff   = taskOff + 4;
	inputOff    = taskOff + 8;
	resultOff   = taskOff + 12;
	tickOff     = taskOff + 16;
	functionOff = taskOff + 20;
	rdyState    = taskOff + 24;
	msgPtrO     = taskOff + 28;
	ticketIdOff = taskOff + 32;
	dbgStrOff   = taskOff + 36;

	taskPtr->cmd      = ApCmd::None;
	taskPtr->status   = ApStatus::Init;
	taskPtr->input    = 0;
	taskPtr->result   = 0;
	taskPtr->tick     = 0;
	taskPtr->funcPtr  = 0;
	taskPtr->ready    = 0;
	taskPtr->msgPtr   = 0;
	taskPtr->ticketId = 0;
	taskPtr->reqInt   = 0;
	taskPtr->numInt   = 0;
	taskPtr->intEax   = 0;
	taskPtr->intEbx   = 0;
	taskPtr->intEcx   = 0;
	taskPtr->intEdx   = 0;

	// Initialisiere die Ringpuffer Pointer ins HIMEM
	uint32_t workerByteSize = (g_workerStackOffset != 0) ? (g_workerStackOffset - 153600 + 4) : 8192;
	taskPtr->resRing.dataArrPhys = workerPhys + workerByteSize + 4096; // HIMEM Result Buffer
	*(uint32_t*)(__djgpp_conventional_base + dataOff + 12) = workerPhys + workerByteSize + 8192; // HIMEM Job Buffer

	void (*tmpInt)(int) = signal(SIGINT, catchSig);
	if (tmpInt != SIG_ERR) oldInt = tmpInt;
	void (*tmpTerm)(int) = signal(SIGTERM, catchSig);
	if (tmpTerm != SIG_ERR) oldTerm = tmpTerm;
	atexit(endTask);
	sysState |= 2;

	// Trampolin aufbauen
	constexpr uint32_t TRAM_SZ = 8192;
	constexpr uint32_t CACHE_L = 64;

	// prepTram ueberschreibt das Layout mit demselben alignPhysical Basis redundante Neuzuweisung bleibt zur Klarheit erhalten
	memoryBase  = alignPhysical;
	payloadBase = alignPhysical + MEM_OFF_PAYLOAD;
    // taskOff and dataOff are already set above workerPhys
	trampolinPage = (uint8_t)((alignPhysical & 0xFF000) >> 12);

	taskPtr = (TaskInt*)(__djgpp_conventional_base + taskOff);

	commandOff  = taskOff + 0;
	statusOff   = taskOff + 4;
	inputOff    = taskOff + 8;
	resultOff   = taskOff + 12;
	tickOff     = taskOff + 16;
	functionOff = taskOff + 20;
	rdyState    = taskOff + 24;
	msgPtrO     = taskOff + 28;
	ticketIdOff = taskOff + 32;
	dbgStrOff   = taskOff + 36;

	_dosmemputb(apStart, TRAM_SZ, memoryBase);

	// IDT aufbauen
	uint32_t idtBase = memoryBase + TRAM_IDT_OFFSET;
	uint32_t baseIsr = memoryBase + (uint32_t)(apIdtIsrStubs - apStart);

	for (int idx = 0; idx < 256; idx++) {
		uint32_t isrAddr = baseIsr + (idx * 16);
		uint32_t entOffs = idtBase + (idx * 8);

		*(uint16_t*)(__djgpp_conventional_base + entOffs + 0) = isrAddr & 0xFFFF;
		*(uint16_t*)(__djgpp_conventional_base + entOffs + 2) = IDT_CS_SELECTOR;
		*(uint8_t*)(__djgpp_conventional_base  + entOffs + 4) = 0x00;
		*(uint8_t*)(__djgpp_conventional_base  + entOffs + 5) = IDT_GATE_FLAGS;
		*(uint16_t*)(__djgpp_conventional_base + entOffs + 6) = (isrAddr >> 16) & 0xFFFF;
	}

	uint32_t patchBase = memoryBase - (uint32_t)apStart;

    uint32_t patchStackVal = g_workerStackOffset;
    if (patchStackVal == 0) {
        patchStackVal = memoryBase + 8192; // fallback if worker loaded manually
    } else {
        patchStackVal = workerPhys + patchStackVal; // Convert offset to absolute physical
    }
    *(uint32_t*)(__djgpp_conventional_base + patchBase + (uint32_t)patchStack) = patchStackVal;
    *(uint32_t*)(__djgpp_conventional_base + patchBase + (uint32_t)patchMain) = workerPhys;
    *(uint32_t*)(__djgpp_conventional_base + patchBase + (uint32_t)patchArg1) = dataOff;
    *(uint32_t*)(__djgpp_conventional_base + patchBase + (uint32_t)patchArg2) = taskOff;

    asm volatile("mfence" ::: "memory");

	// Trampolin Bereich aus dem BSP Cache herausschreiben bevor der AP startet
	for (uint32_t addr = memoryBase; addr < memoryBase + TRAM_SZ; addr += CACHE_L) {
		asm volatile(
			"push %%fs \n\t"
			"mov %1, %%fs \n\t"
			"clflush %%fs:(%0) \n\t"
			"pop %%fs"
			: : "r"(addr), "rm"((unsigned short)_dos_ds)
			: "memory"
		);
	}
	asm volatile("mfence" ::: "memory");

	printf("[setupSys] memBase=0x%08X workerEntry=0x%08X\n", memoryBase, workerPhys);
	printf("[setupSys] arg1=0x%08X arg2=0x%08X\n", dataOff, taskOff);

	sysState |= 4;
	return true;
}

// Clear VGA cells used by AP exception handler and worker status
// Prevents false panic triggers on AP boot
static void clearPanicVga() {
	for (int c = 0; c < 80; c++) {
		_farpokew(_dos_ds, VGA_TEXT_BASE + c * 2, VGA_BLANK_CELL);
	}
	asm volatile("mfence" ::: "memory");
}

uint32_t initMem() {
	constexpr uint32_t MEM_ALC = 4096;
	constexpr uint32_t PAGEMSK = 4095;

	int dosSel;
	int dosSeg = __dpmi_allocate_dos_memory(MEM_ALC, &dosSel);
	if (dosSeg == -1) {
		exit(1);
	}
	uint32_t physicalBase = (uint32_t)dosSeg * 16;
	return (physicalBase + PAGEMSK) & ~PAGEMSK;
}



uint32_t CoreTwoAPI::startAp(uint8_t apicId) {
	// Kandidaten: zuerst die uebergebene Nummer, bei Timeout einmalig die
	// naechst hoehere ID (apicId + 1). Zweiter Eintrag entfaellt bei Overflow.
	uint8_t cand[2];
	int nCand = 0;
	cand[nCand++] = apicId;
	if (apicId != 0xFF) cand[nCand++] = (uint8_t)(apicId + 1);
 
	for (int ci = 0; ci < nCand; ci++) {
		uint8_t cur = cand[ci];
 
		// Dieselbe ID die nachweislich den AP gebootet hat fuer alle
		// spaeteren NMI Wakes und INIT IPIs wiederverwenden
		g_apId = cur;
 
		// Clear old AP panic signature to prevent false alarms
		clearPanicVga();
 
		// Reset handshake to prevent misinterpreting old AP status
		taskPtr->status = ApStatus::Init;
		asm volatile("mfence" ::: "memory");
 
		// Enable Local APIC and Spurious Vector
		apic_.enable();
 
		printf("[startAp] cand=%u init assert...\n", cur); fflush(stdout);
		// INIT IPI assert
		apic_.sendInitAssert(cur);
		apic_.waitIdle();
		microWait(10000);
 
		printf("[startAp] init deassert...\n"); fflush(stdout);
		// INIT IPI deassert
		apic_.sendInitDeassert(cur);
		apic_.waitIdle();
		microWait(10000);
 
		printf("[startAp] SIPI 1...\n"); fflush(stdout);
		// First SIPI
		apic_.sendSipi(cur, trampolinPage);
		apic_.waitIdle();
		microWait(2000);
 
		printf("[startAp] SIPI 2...\n"); fflush(stdout);
		// Second SIPI
		apic_.sendSipi(cur, trampolinPage);
		apic_.waitIdle();
		microWait(10000);
 
		printf("[startAp] waiting for AP...\n"); fflush(stdout);
		// Auf AP warten 
		uint32_t strtTck = getBiosTick();
		uint32_t spins   = 0;
		bool timedOut    = false;
		while (status() != ApStatus::Ready) {
			if (apPanicReport(taskPtr, statusOff, taskOff)) {
				printf("[startAp] AP PANIK waehrend Boot (Details oben / panic.txt)\n");
				fflush(stdout);
				exit(1);
			}
			if ((getBiosTick() - strtTck > 90) || (++spins > 300000000u)) {
				timedOut = true;
				break;
			}
			asm volatile("pause");
		}
 
		if (!timedOut) {
			// AP lebt. Echte LAPIC-APIC-ID. Vor dem Lesen flushen sonst sieht der BSP evtl 0.
			bspClflush(taskOff + 552);            // intEbx-Zeile invalidieren
			uint32_t reportedId = taskPtr->intEbx;
			if (reportedId != 0 && reportedId < 0x100) {
				g_apId = (uint8_t)reportedId;
			}
			printf("[startAp] AP alive auf APIC-ID %u (self-reported %lu)\n",
			       (unsigned)cur, (unsigned long)reportedId);
			fflush(stdout);
			return 1;
		}
 
		// Timeout auf dieser ID ck naechst hoehere ID
		if (ci + 1 < nCand) {
			printf("[startAp] APIC-ID %u kam nicht hoch -> versuche %u\n",
			       (unsigned)cur, (unsigned)cand[ci + 1]);
			fflush(stdout);
		}
	}
 
	printf("[startAp] TIMEOUT: kein AP alive (versucht %u und %u)\n",
	       (unsigned)apicId, (unsigned)(apicId == 0xFF ? apicId : apicId + 1));
	fflush(stdout);
	return 0;
}

void CoreTwoAPI::spawn(uint32_t physAddr) {
	if ((sysState & 6) != 6 || physAddr == 0) return;

	taskPtr->funcPtr = physAddr;
	taskPtr->input = dataOff;

	asm volatile("lock; addl $0, (%%esp)" ::: "memory");
	taskPtr->cmd = ApCmd::Spawn;
	asm volatile("" ::: "memory");
}

void CoreTwoAPI::pollDbg(uint32_t flagVal) {
	// backward compatibility
}

ApStatus CoreTwoAPI::status() {
	// AP Handshake Write aus dem RAM neu laden seltener flushen
	static uint32_t flushCount = 0;
	if (flushCount++ % 1000 == 0) {
		bspClflush(statusOff);
	}
	return taskPtr->status;
}


uint32_t CoreTwoAPI::getRes() {
	return taskPtr->result;
}

uint32_t CoreTwoAPI::getTick() {
	return taskPtr->tick;
}

uint32_t CoreTwoAPI::getBiosTick() {
	return _farpeekl(_dos_ds, BIOS_TICK_OFFSET);
}

void CoreTwoAPI::finish() {
	taskPtr->cmd = ApCmd::Stop;
	asm volatile("" ::: "memory");

	wakeAp();

	uint32_t waitTime = 0;
	while (status() != ApStatus::Finished && waitTime < 10000000) {
		taskPtr->cmd = ApCmd::Stop;
		asm volatile("pause");
		waitTime++;
	}

	if (status() != ApStatus::Finished) {
		apic_.sendInitNmiStop();
	}
}

void CoreTwoAPI::sendData(void *sourcePointer, uint32_t dataSize) {
	_dosmemputb(sourcePointer, dataSize, dataOff);
}

void CoreTwoAPI::wakeAp() {
	// Unsere Enqueue Schreibzugriffe muessen global sichtbar sein BEVOR wir das
	// asleep Flag lesen Store Load Barriere Dekker Seite des Producers
	asm volatile("mfence" ::: "memory");

	// Fix 3 Lost Wakeup Race Der AP kann einen NMI im Fenster
	// von gesetztem asleep Flag bis zum hlt Befehl verpassen
	// Deshalb NMI so lange wiederholen bis der AP den Wake quittiert
	// Ist der Kern schon wach ist das sofort ein No Op
	for (int attempt = 0; attempt < 1000; attempt++) {
		bspClflush(taskOff + 556);
		if (taskPtr->intEcx != 1) return;
		apic_.waitIdle();
		apic_.sendNmi(g_apId);
		for (int s = 0; s < 20000; s++) {
			if (s % 1000 == 0) {
				bspClflush(taskOff + 556);
			}
			if (taskPtr->intEcx != 1) return;
			asm volatile("pause");
		}
	}
}

void catchSig(int sigNum) {
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	if (globTask != NULL) {
		globTask->finish();
	}

	if (sigNum == SIGINT && oldInt != NULL && oldInt != SIG_DFL && oldInt != SIG_IGN) {
		oldInt(sigNum);
	} else if (sigNum == SIGTERM && oldTerm != NULL && oldTerm != SIG_DFL && oldTerm != SIG_IGN) {
		oldTerm(sigNum);
	} else {
		exit(1);
	}
}

/*
uint32_t allocDpmi(uint32_t allocSize) {
	__dpmi_meminfo memInfo;
	memInfo.size = allocSize;
	if (__dpmi_allocate_memory(&memInfo) != 0) {
		return 0; // Failed
	}
	if (__dpmi_lock_linear_region(&memInfo) != 0) {
        __dpmi_free_memory(memInfo.handle);
        return 0; // Failed to lock
    }
	return memInfo.address;
}
*/

void CoreTwoAPI::sendDataOffset(void *sourcePointer, uint32_t dataSize, uint32_t offset) {
	_dosmemputb(sourcePointer, dataSize, dataOff + offset);
}

uint32_t CoreTwoAPI::getHeadIdx() {
	return _farpeekl(_dos_ds, dataOff + 0);
}

uint32_t CoreTwoAPI::getDataOff() {
	return dataOff;
}

uint32_t CoreTwoAPI::getPayloadBase() {
	return payloadBase;
}

#include <dpmi.h>
uint32_t allocXMS(uint32_t size_bytes, uint16_t* outHandle) {
    if (outHandle) *outHandle = 0;

    // XMS-Treiber-Einsprungpunkt sicherstellen (initXms macht den 0x4300-Check)
    if (xms_entry == 0) {
        initXms();
        if (xms_entry == 0) return 0;
    }

    uint32_t size_kb = (size_bytes + 1023) / 1024;
    __dpmi_regs r;

    // XMS 0x09: High-Memory-Block anfordern
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0900;
    r.x.dx = (uint16_t)size_kb;
    r.x.cs = xms_entry >> 16;
    r.x.ip = xms_entry & 0xFFFF;
    if (__dpmi_simulate_real_mode_procedure_retf(&r) != 0) return 0;
    if (r.x.ax != 1) return 0;
    uint16_t xms_handle = r.x.dx;

    // XMS 0x0C: Block sperren -> physische Adresse in DX:BX
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0C00;
    r.x.dx = xms_handle;
    r.x.cs = xms_entry >> 16;
    r.x.ip = xms_entry & 0xFFFF;
    if (__dpmi_simulate_real_mode_procedure_retf(&r) != 0) return 0;
    if (r.x.ax != 1) return 0;

    if (outHandle) *outHandle = xms_handle;
    return ((uint32_t)r.x.dx << 16) | r.x.bx;
}
