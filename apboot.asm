; Application Processor Boot Trampoline
; Boots secondary processors in a multiprocessor environment
; Begins execution in 16 bit real mode and creates a basic Global Descriptor Table
; Dynamically calculates and patches physical base addresses for GDT and IDT based on the runtime code segment
; Transitions the processor into 32 bit protected mode with a flat memory model
; Enables floating point unit and SSE instructions via control register configuration
; Installs a full 256-entry ISR table that catches every CPU exception (0-31)
; On a fault the handler captures the vector, CPU error code, faulting EIP and CR2 and the EFLAGS,
; reports them back to the producer via the TaskInt structure (status=0xEE panic marker, intEax/intEcx/intEdx/intEsi/intEdi)

global _apStart
global _apEnd
global _apIdtIsrStubs
global _patchMain
global _patchArg1
global _patchArg2
global _patchStack
global _patchExcSlot

section .text
bits 16
_apStart:
	cli
	cld

	mov ax, cs
	mov ds, ax

	xor eax, eax
	mov ax, cs
	shl eax, 4
	mov ebx, eax

	mov eax, ebx
	add eax, _gdt - _apStart
	mov dword [_patchGdt - _apStart], eax

	; Patch 0x08 Code Descriptor to have base=ebx (memoryBase)
	mov word [_gdt - _apStart + 8 + 2], bx
	mov eax, ebx
	shr eax, 16
	mov byte [_gdt - _apStart + 8 + 4], al
	mov byte [_gdt - _apStart + 8 + 7], ah

	; 0x10 and 0x18 are FLAT 0-based.
	; We don't patch 0x20 and 0x28 here yet, we do it in 32-bit flat mode!

	lgdt [cs:_gdtPtr - _apStart]

	mov eax, cr0
	or eax, 1
	mov cr0, eax

	; Short jump to clear prefetch queue (VirtualBox bug mitigation)
	jmp short .flush_prefetch
.flush_prefetch:
	db 0x66
	db 0xEA
	dd _ap32 - _apStart
	dw 0x0008

bits 32
_ap32:
	; Now in CS=0x08 (base memoryBase).
	; Switch completely to FLAT descriptors (0x10 Data, 0x18 Code)
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	; Initialize ESP to the absolute stack top (valid for SS=0x10)
	mov esp, [ebx + _patchStack - _apStart]
	test esp, esp
	jnz .have_stack
	mov esp, ebx
	add esp, 8192
.have_stack:

	; Jump to CS=0x18 (FLAT)
	mov eax, ebx
	add eax, .flat_mode - _apStart
	push dword 0x18
	push eax
	retf

.flat_mode:
	; Now in FULL FLAT MODE (CS=0x18, DS=0x10). All addresses must be absolute!
	; ebx is still memoryBase.

	; Patch IDT Base
	mov eax, ebx
	add eax, 5120
	mov dword [ebx + _patchIdtBase - _apStart], eax

	; Load IDT
	mov eax, ebx
	add eax, _idtPtr - _apStart
	lidt [eax]

	; Exception-Handler: absolute phys. Adresse von _patchArg2 (= taskAddress)
	; in den moffs-Slot des Handlers schreiben.
	mov eax, ebx
	add eax, _patchArg2 - _apStart
	mov dword [ebx + _patchExcSlot - _apStart], eax

	; --- FPU/SSE freischalten ---
	mov eax, cr0
	and eax, 0x9FFFFFFB
	or  eax, 0x00000002
	mov cr0, eax

	fninit
	mov eax, cr4
	or eax, 0x00000600
	mov cr4, eax

	; --- AVX/XSAVE enable, CPUID-guarded ---
	; Only touch OSXSAVE/XCR0 if the CPU actually has XSAVE, and only
	; enable the AVX state bit if AVX is present. Blindly setting
	; CR4.OSXSAVE on a non-XSAVE CPU faults #GP; xgetbv/xsetbv without
	; OSXSAVE fault #UD; enabling XCR0.AVX without AVX faults #GP.
	; cpuid clobbers ebx (= memoryBase) and ecx, so save/restore them.
	push ebx
	mov eax, 1
	cpuid                        ; ecx = feature flags
	mov esi, ecx                 ; stash flags (cr4 write / xgetbv reuse ecx)
	pop ebx                      ; restore memoryBase

	test esi, 1 << 26            ; XSAVE supported?
	jz .no_xsave                 ; no -> leave OSXSAVE/XCR0 untouched

	mov eax, cr4
	or  eax, 0x00040000          ; CR4.OSXSAVE (bit 18)
	mov cr4, eax

	xor ecx, ecx
	xgetbv                       ; edx:eax = XCR0
	or  eax, 0x3                 ; x87 | SSE (always safe once XSAVE is on)
	test esi, 1 << 28            ; AVX supported?
	jz .wr_xcr0
	or  eax, 0x4                 ; + AVX state
.wr_xcr0:
	xor edx, edx
	xsetbv
.no_xsave:

	; --- Patch 0x20 and 0x28 with workerPhys ---
	; workerPhys is stored in _patchMain
	mov ecx, [ebx + _patchMain - _apStart] ; ecx = workerPhys

	mov eax, ebx
	add eax, _gdt - _apStart ; eax = address of GDT

	; Patch 0x20
	mov edx, ecx
	mov word [eax + 0x20 + 2], dx
	shr edx, 16
	mov byte [eax + 0x20 + 4], dl
	mov byte [eax + 0x20 + 7], dh

	; Patch 0x28
	mov edx, ecx
	mov word [eax + 0x28 + 2], dx
	shr edx, 16
	mov byte [eax + 0x28 + 4], dl
	mov byte [eax + 0x28 + 7], dh

	; Switch segment registers to the new worker segments
	mov edx, esp
	sub edx, ecx ; Calculate relative ESP

	mov ax, 0x20
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov esp, edx ; Safe switch (interrupts delayed after mov ss)

	; Leave FS/GS as 0x10 flat! Very important for APIC accesses via fs:
	mov ax, 0x10
	mov fs, ax
	mov gs, ax

	; Push arguments (Right to Left)
	; Arg3: workerPhys (ecx)
	push ecx
	; Arg2: taskAddr (use fs: because DS is now workerPhys)
	push dword [fs:ebx + _patchArg2 - _apStart]
	; Arg1: dataPhys (use fs: because DS is now workerPhys)
	push dword [fs:ebx + _patchArg1 - _apStart]

	; --- DEBUG: Print 'OK' in GREEN at the bottom right (24th row, 78th col) ---
	push eax
	push edi
	push es
	mov ax, 0x10
	mov es, ax
	mov edi, 0xB8000
	mov word [es:edi + 3996], 0x2F4F ; 'O'
	mov word [es:edi + 3998], 0x2F4B ; 'K'
	pop es
	mov ax, 0x20
	mov es, ax
	pop edi
	pop eax
	
	; Dummy return address
	push dword 0x00000000

	; Far Jump to Worker (CS=0x28, Offset=0)
	; We push CS and EIP and use retf
	push dword 0x28
	push dword 0x00000000
	retf

apLoop:
	cli
	hlt
	jmp apLoop

; ==============================================================
; PATCH-SLOTS 
;   _patchStack : ABSOLUTER Stack-Top (wird in _ap32 unter SS=0x10 flat geladen,
;                 in .flat_mode dann per (esp - workerPhys) auf SS=0x20 umgerechnet).
;   _patchArg1  : dataPhys  (absolut)
;   _patchArg2  : taskAddr  (absolut)
;   _patchMain  : workerPhys (= Ladeadresse = C-Entry)
; ==============================================================
align 4
_patchStack:
	dd 0x00000000
_patchArg2:
	dd 0x00000000
_patchArg1:
	dd 0x00000000
_patchMain:
	dd 0x00000000

; ==============================================================
; DATENSTRUKTUREN 
; ==============================================================
align 4
_idtPtr:
	dw 0x07FF
_patchIdtBase:
	dd 0x00000000

align 4
_gdtPtr:
	dw 47
_patchGdt:
	dd 0x00000000

align 8
_gdt:
	dq 0x0000000000000000

	dw 0xFFFF
	dw 0x0000
	db 0x00
	db 0x9A
	db 0xCF
	db 0x00

	dq 0x00CF92000000FFFF ; 0x10 Data 32 Flat
	dq 0x00CF9A000000FFFF ; 0x18 Code 32 Flat

	dq 0x00CF92000000FFFF ; 0x20 Data 32 base=workerPhys
	dq 0x00CF9A000000FFFF ; 0x28 Code 32 base=workerPhys
; ==============================================================

; ==============================================================
; IDT-ISR-Stubs.
; ==============================================================
align 16
_apIdtIsrStubs:
%assign i 0
%rep 256
    %if i == 2
        iretd
        align 16
    %elif i == 255
        iretd
        align 16
    %elif i >= 32
        push eax
        mov eax, 0xFEE000B0
        mov dword [fs:eax], 0 ; use FS (flat) for APIC access
        pop eax
        iretd
        align 16
    %else
        %if i==8 || i==10 || i==11 || i==12 || i==13 || i==14 || i==17
            push byte i
        %else
            push byte 0
            push byte i
        %endif
        jmp _exc_common
        align 16
    %endif
%assign i i+1
%endrep

_exc_common:
	cli
	; Switch DS/ES to Flat 0x10 to access VGA and TaskInt
	push eax
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	pop eax

	; VGA
	mov edi, 0xB8000
	mov ecx, 80 * 25
	mov ax, 0x4F20
	rep stosw

	mov edi, 0xB8000 + (24 * 160)
	mov word [edi+0], 0x4F45
	mov word [edi+2], 0x4F58
	mov word [edi+4], 0x4F43
	mov word [edi+6], 0x4F3D
	add edi, 8
	mov bl, [esp + 0]
	call _putByteHex

	mov word [edi+0], 0x4F20
	mov word [edi+2], 0x4F45
	mov word [edi+4], 0x4F52
	mov word [edi+6], 0x4F52
	mov word [edi+8], 0x4F3D
	add edi, 10
	mov eax, [esp + 4]
	call _putDwordHex

	mov word [edi+0], 0x4F20
	mov word [edi+2], 0x4F45
	mov word [edi+4], 0x4F49
	mov word [edi+6], 0x4F50
	mov word [edi+8], 0x4F3D
	add edi, 10
	mov eax, [esp + 8]
	call _putDwordHex

	mov word [edi+0], 0x4F20
	mov word [edi+2], 0x4F43
	mov word [edi+4], 0x4F52
	mov word [edi+6], 0x4F32
	mov word [edi+8], 0x4F3D
	add edi, 10
	mov eax, cr2
	call _putDwordHex

	; TaskInt Report
	db 0xA1
_patchExcSlot:
	dd 0x00000000
	mov edi, eax

	mov eax, [esp + 0]
	mov [edi + 572], eax
	mov eax, [esp + 4]
	mov [edi + 556], eax
	mov eax, [esp + 8]
	mov [edi + 560], eax
	mov eax, cr2
	mov [edi + 564], eax
	mov eax, [esp + 16]
	mov [edi + 568], eax
	mov dword [edi + 4], 0x000000EE

	mfence

.hang:
	cli
	hlt
	jmp .hang

_putByteHex:
	mov al, bl
	shr al, 4
	call _nibble
	mov dh, 0x4F
	mov [edi], dx
	add edi, 2
	mov al, bl
	and al, 0x0F
	call _nibble
	mov dh, 0x4F
	mov [edi], dx
	add edi, 2
	ret

_putDwordHex:
	mov ebx, eax
	mov ecx, 8
.lp:
	rol ebx, 4
	mov al, bl
	call _nibble
	mov dh, 0x4F
	mov [edi], dx
	add edi, 2
	dec ecx
	jnz .lp
	ret

_nibble:
	and al, 0x0F
	cmp al, 9
	jbe .dig
	add al, 7
.dig:
	add al, '0'
	mov dl, al
	ret

_apEnd:
