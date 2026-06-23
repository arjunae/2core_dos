#include "vesa.h"
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/nearptr.h>
#include <sys/farptr.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

// SSE2 lokal erzwingen, falls der Build vesa.cc ohne -msse2 uebersetzt.
// Muss VOR emmintrin.h stehen, sonst sind die Intrinsics nicht verfuegbar.
#pragma GCC push_options
#pragma GCC target("sse2")
#include <emmintrin.h>   // SSE2: _mm_stream_si128, _mm_loadu_si128, _mm_sfence

// WC-Burst-Kopie via Non-Temporal Stores. Auf einer VRAM-Apertur (UC per
// Default-MTRR) buendelt der WC-Puffer der CPU 64-Byte-Bursts, die die
// Northbridge am Stueck zur GPU schiebt -> oft 5-15x schneller als ein
// gewoehnliches memcpy in den uncached LFB. Ziel muss 16-Byte-aligned sein
// (movntdq), Quelle darf unaligned sein.
__attribute__((target("sse2")))
static void memcpy_wc(void *dst, const void *src, size_t len) {
	uint8_t *d = (uint8_t*)dst;
	const uint8_t *s = (const uint8_t*)src;
	size_t i = 0;

	// Prolog: bis zur 16-Byte-Grenze des ZIELS skalar (movntdq verlangt aligned dst)
	while ((((uintptr_t)(d + i)) & 15) && i < len) { d[i] = s[i]; i++; }

	for (; i + 63 < len; i += 64) {
		_mm_prefetch((const char*)(s + i + 256), _MM_HINT_NTA);
		__m128i r0 = _mm_loadu_si128((const __m128i*)(s + i +  0));
		__m128i r1 = _mm_loadu_si128((const __m128i*)(s + i + 16));
		__m128i r2 = _mm_loadu_si128((const __m128i*)(s + i + 32));
		__m128i r3 = _mm_loadu_si128((const __m128i*)(s + i + 48));
		_mm_stream_si128((__m128i*)(d + i +  0), r0);
		_mm_stream_si128((__m128i*)(d + i + 16), r1);
		_mm_stream_si128((__m128i*)(d + i + 32), r2);
		_mm_stream_si128((__m128i*)(d + i + 48), r3);
	}
	for (; i < len; i++) d[i] = s[i];

	_mm_sfence();   // NT-Stores sind schwach geordnet -> sichtbar machen
}
#pragma GCC pop_options

static inline uint64_t rdtsc64(void) {
	uint32_t lo, hi;
	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

#define DOS_MEM_BASE 0x0
#define VESA_INFO_PTR 0x2000 // use a small buffer in conventional memory (e.g. at 0x2000)

typedef struct __attribute__((packed)) {
    uint16_t attributes;
    uint8_t winA, winB;
    uint16_t granularity;
    uint16_t winsize;
    uint16_t segmentA, segmentB;
    uint32_t realFctPtr;
    uint16_t pitch;
    uint16_t x_res, y_res;
    uint8_t w_char, y_char, planes, bpp, banks;
    uint8_t memory_model, bank_size, image_pages;
    uint8_t reserved0;
    uint8_t red_mask, red_position;
    uint8_t green_mask, green_position;
    uint8_t blue_mask, blue_position;
    uint8_t rsv_mask, rsv_position;
    uint8_t directcolor_attributes;
    uint32_t physbase;
    uint32_t offscreen_mem_offset;
    uint16_t offscreen_mem_size;
    uint8_t reserved1[206];
} VbeModeInfo;

int vesa_pitch = 0;
int vesa_bpp = 0;
uint32_t vesa_phys_base = 0;

static int clamp(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return val;
}

// VBE-Mode-Liste dumpen: jeden Modus mit gewuenschter Aufloesung samt bpp,
// LFB-Verfuegbarkeit und Mode-Nummer ausgeben. Aendert den Grafikmodus NICHT.
// Vor vesa_init aufrufen; Ausgabe erscheint in der finalen Textausgabe.
void vesa_dump_modes(int want_w, int want_h) {
    if (!__djgpp_nearptr_enable()) return;

    __dpmi_regs r;
    int tb_seg = _go32_info_block.linear_address_of_transfer_buffer >> 4;
    int tb_lin = _go32_info_block.linear_address_of_transfer_buffer;

    // VBE Controller Info (Funktion 0x4F00) holen -> Zeiger auf Mode-Liste
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F00;
    r.x.es = tb_seg;
    r.x.di = 0;
    // "VBE2"-Signatur anfordern (manche BIOSe liefern dann mehr)
    _farpokel(_dos_ds, tb_lin, 0x32454256); // "VBE2"
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) { printf("VBE Controller Info failed\n"); return; }

    // VbeInfoBlock: video_mode_ptr liegt bei Offset 14 (seg:off Realmode-Zeiger)
    uint16_t mode_off = _farpeekw(_dos_ds, tb_lin + 14);
    uint16_t mode_seg = _farpeekw(_dos_ds, tb_lin + 16);
    uint32_t mode_lin = ((uint32_t)mode_seg << 4) + mode_off;

    printf("--- VBE modes %dx%d ---\n", want_w, want_h);
    // Mode-Infos in die ZWEITE Haelfte des Transfer-Buffers schreiben,
    // damit die Mode-Liste (erste Haelfte) nicht ueberschrieben wird.
    int info_seg = (tb_lin + 512) >> 4;
    int info_lin = tb_lin + 512;

    for (int i = 0; i < 256; i++) {
        uint16_t m = _farpeekw(_dos_ds, mode_lin + i * 2);
        if (m == 0xFFFF) break;

        memset(&r, 0, sizeof(r));
        r.x.ax = 0x4F01;
        r.x.cx = m;
        r.x.es = info_seg;
        r.x.di = 0;
        __dpmi_int(0x10, &r);
        if (r.x.ax != 0x004F) continue;

        uint16_t attrs = _farpeekw(_dos_ds, info_lin + 0);   // mode attributes
        uint16_t xres  = _farpeekw(_dos_ds, info_lin + 18);  // x_res
        uint16_t yres  = _farpeekw(_dos_ds, info_lin + 20);  // y_res
        uint8_t  bpp   = _farpeekb(_dos_ds, info_lin + 25);  // bits_per_pixel
        uint32_t pbase = _farpeekl(_dos_ds, info_lin + 40);  // phys base (LFB)

        if (xres == want_w && yres == want_h) {
            printf("  mode %x: %dx%d %dbpp LFB=%s base=%x\n",
                   m, xres, yres, bpp,
                   (attrs & 0x80) ? "yes" : "no", pbase);
        }
    }
    printf("-----------------------\n");
    fflush(stdout);
}

// Sucht den besten 1024x768-LFB-Modus: bevorzugt 32bpp, faellt auf 24bpp
// zurueck. Liefert die Mode-Nummer (ohne LFB-Bit) und fuellt out_bpp/out_pitch/
// out_base. 0 = nichts gefunden.
static uint16_t vesa_find_mode(int want_w, int want_h,
                               uint8_t* out_bpp, uint16_t* out_pitch, uint32_t* out_base) {
    __dpmi_regs r;
    int tb_lin = _go32_info_block.linear_address_of_transfer_buffer;
    int tb_seg = tb_lin >> 4;

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F00;
    r.x.es = tb_seg;
    r.x.di = 0;
    _farpokel(_dos_ds, tb_lin, 0x32454256); // "VBE2"
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return 0;

    uint16_t mode_off = _farpeekw(_dos_ds, tb_lin + 14);
    uint16_t mode_seg = _farpeekw(_dos_ds, tb_lin + 16);
    uint32_t mode_lin = ((uint32_t)mode_seg << 4) + mode_off;

    int info_seg = (tb_lin + 512) >> 4;
    int info_lin = tb_lin + 512;

    uint16_t best32 = 0, best24 = 0;
    uint8_t  bpp32 = 0, bpp24 = 0;
    uint16_t pitch32 = 0, pitch24 = 0;
    uint32_t base32 = 0, base24 = 0;

    for (int i = 0; i < 256; i++) {
        uint16_t m = _farpeekw(_dos_ds, mode_lin + i * 2);
        if (m == 0xFFFF) break;

        memset(&r, 0, sizeof(r));
        r.x.ax = 0x4F01;
        r.x.cx = m;
        r.x.es = info_seg;
        r.x.di = 0;
        __dpmi_int(0x10, &r);
        if (r.x.ax != 0x004F) continue;

        uint16_t attrs = _farpeekw(_dos_ds, info_lin + 0);
        uint16_t xres  = _farpeekw(_dos_ds, info_lin + 18);
        uint16_t yres  = _farpeekw(_dos_ds, info_lin + 20);
        uint16_t pitch = _farpeekw(_dos_ds, info_lin + 16);
        uint8_t  bpp   = _farpeekb(_dos_ds, info_lin + 25);
        uint32_t pbase = _farpeekl(_dos_ds, info_lin + 40);

        if (xres != want_w || yres != want_h) continue;
        if (!(attrs & 0x80) || pbase == 0) continue;   // LFB noetig

        if (bpp == 32 && !best32) { best32 = m; bpp32 = bpp; pitch32 = pitch; base32 = pbase; }
        if (bpp == 24 && !best24) { best24 = m; bpp24 = bpp; pitch24 = pitch; base24 = pbase; }
    }

    if (best32) { *out_bpp = bpp32; *out_pitch = pitch32; *out_base = base32; return best32; }
    if (best24) { *out_bpp = bpp24; *out_pitch = pitch24; *out_base = base24; return best24; }
    return 0;
}

uint32_t* vesa_init(int req_width, int req_height) {
    if (!__djgpp_nearptr_enable()) return NULL;

    __dpmi_regs r;

    // Besten Modus automatisch waehlen (32bpp bevorzugt). Loest die
    // Notebook(0x118=32)-vs-VM(0x118=24, 0x144=32)-Divergenz: jede Maschine
    // bekommt ihren eigenen 32bpp-Modus, derselbe Build laeuft auf beiden.
    uint8_t  found_bpp = 0;
    uint16_t found_pitch = 0;
    uint32_t found_base = 0;
    uint16_t modeNum = vesa_find_mode(req_width, req_height, &found_bpp, &found_pitch, &found_base);
    if (!modeNum) {
        printf("No suitable %dx%d LFB mode found!\n", req_width, req_height);
        return NULL;
    }

    uint16_t mode = modeNum | 0x4000;   // LFB-Bit
    uint32_t phys_base = found_base;
    vesa_phys_base = phys_base;
    vesa_pitch = found_pitch;
    vesa_bpp = found_bpp;

    printf("VESA: mode %x  %dbpp  pitch %d  base %x\n", modeNum, found_bpp, found_pitch, phys_base);

    if (phys_base == 0) {
        printf("No LFB available for this mode!\n");
        return NULL;
    }

    // Map physical memory (safe size using actual pitch)
    __dpmi_meminfo mem;
    mem.address = phys_base;
    mem.size = (uint32_t)vesa_pitch * req_height;
    if (__dpmi_physical_address_mapping(&mem) != 0) {
        printf("Failed to map LFB!\n");
        return NULL;
    }

    // Set mode
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F02;
    r.x.bx = mode;
    __dpmi_int(0x10, &r);

    return (uint32_t *)(mem.address + __djgpp_conventional_base);
}

#include <conio.h>

void vesa_restore_text_mode() {
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
    
    // Fallback/Reinforcement using conio.h
    textmode(C80);
    clrscr();
}

void vesa_wait_vsync() {
    while (inportb(0x3DA) & 8);
    while (!(inportb(0x3DA) & 8));
}

// ============================================================================
// Umschalter: 1 = SSE-Single-Pass direkt in den LFB (nur 32bpp), 0 = Skalar
// ============================================================================
#define USE_SSE_CONVERT 1

#if USE_SSE_CONVERT
// SSE-Konvertierung von 8 YUV-Pixeln -> 8 BGRA-Pixel (32bpp), exakt die
// Shift-Approximation aus dem Skalarpfad nachgebaut, direkt per NT-Store
// in den LFB geschrieben. Single-Pass: kein backbuffer, kein zweiter memcpy.
//
//   R = Y + (V + V>>2 + V>>3 + V>>5)
//   G = Y - (U>>1 + U>>4 + U>>5) - (V>>1 + V>>3 + V>>4 + V>>5)
//   B = Y + (U + U>>1 + U>>2 + U>>6)
//   Pixel = (R<<16)|(G<<8)|B   (also Speicher-Bytes B,G,R,0)
//
// Arithmetik in 16-bit signed Lanes (8 parallel), Saettigung via packuswb.
__attribute__((target("sse2")))
static inline __m128i shr_s16(__m128i v, int n) { return _mm_srai_epi16(v, n); }

__attribute__((target("sse2")))
static void convert_row_sse_32(uint8_t* dstLFB,
                               const uint8_t* y_row,
                               const uint8_t* u_row,
                               const uint8_t* v_row,
                               int w) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i c128 = _mm_set1_epi16(128);

    int x = 0;
    for (; x + 8 <= w; x += 8) {
        // 8 Y-Bytes -> 16-bit
        __m128i Y8 = _mm_loadl_epi64((const __m128i*)(y_row + x));
        __m128i Y  = _mm_unpacklo_epi8(Y8, zero);                 // 8x Y (0..255)

        // 4 U/V-Bytes (Chroma halb so breit) -> auf 8 duplizieren
        __m128i U4 = _mm_cvtsi32_si128(*(const int*)(u_row + x/2));
        __m128i V4 = _mm_cvtsi32_si128(*(const int*)(v_row + x/2));
        U4 = _mm_unpacklo_epi8(U4, zero);                         // 4x U als 16-bit
        V4 = _mm_unpacklo_epi8(V4, zero);
        __m128i U = _mm_unpacklo_epi16(U4, U4);                   // dupliziert: u0u0u1u1u2u2u3u3
        __m128i V = _mm_unpacklo_epi16(V4, V4);
        U = _mm_sub_epi16(U, c128);                              // U-128 (signed)
        V = _mm_sub_epi16(V, c128);

        // V_R = V + V>>2 + V>>3 + V>>5
        __m128i V_R = _mm_add_epi16(V, _mm_add_epi16(_mm_add_epi16(shr_s16(V,2), shr_s16(V,3)), shr_s16(V,5)));
        // U_B = U + U>>1 + U>>2 + U>>6
        __m128i U_B = _mm_add_epi16(U, _mm_add_epi16(_mm_add_epi16(shr_s16(U,1), shr_s16(U,2)), shr_s16(U,6)));
        // U_G = U>>1 + U>>4 + U>>5
        __m128i U_G = _mm_add_epi16(_mm_add_epi16(shr_s16(U,1), shr_s16(U,4)), shr_s16(U,5));
        // V_G = V>>1 + V>>3 + V>>4 + V>>5
        __m128i V_G = _mm_add_epi16(_mm_add_epi16(shr_s16(V,1), shr_s16(V,3)), _mm_add_epi16(shr_s16(V,4), shr_s16(V,5)));

        __m128i R = _mm_add_epi16(Y, V_R);
        __m128i B = _mm_add_epi16(Y, U_B);
        __m128i G = _mm_sub_epi16(Y, _mm_add_epi16(U_G, V_G));

        // Saettigung auf 0..255 (packus signed16 -> unsigned8), dann wieder spreizen
        __m128i R8 = _mm_packus_epi16(R, R);   // 8x R in low 8 bytes
        __m128i G8 = _mm_packus_epi16(G, G);
        __m128i B8 = _mm_packus_epi16(B, B);

        // BGRA interleaven: B,G,R,0  pro Pixel
        __m128i BG = _mm_unpacklo_epi8(B8, G8);          // B0 G0 B1 G1 ...
        __m128i RA = _mm_unpacklo_epi8(R8, zero);        // R0 0  R1 0  ...
        __m128i px0 = _mm_unpacklo_epi16(BG, RA);        // B0 G0 R0 0  B1 G1 R1 0  B2..B3
        __m128i px1 = _mm_unpackhi_epi16(BG, RA);        // B4..B7

        _mm_stream_si128((__m128i*)(dstLFB + (x + 0) * 4), px0);
        _mm_stream_si128((__m128i*)(dstLFB + (x + 4) * 4), px1);
    }

    // Rest skalar (w nicht durch 8 teilbar)
    for (; x < w; x++) {
        int Y = y_row[x];
        int U = u_row[x/2] - 128;
        int V = v_row[x/2] - 128;
        int R = Y + (V + (V>>2) + (V>>3) + (V>>5));
        int G = Y - ((U>>1)+(U>>4)+(U>>5)) - ((V>>1)+(V>>3)+(V>>4)+(V>>5));
        int B = Y + (U + (U>>1) + (U>>2) + (U>>6));
        R = R<0?0:(R>255?255:R); G = G<0?0:(G>255?255:G); B = B<0?0:(B>255?255:B);
        ((uint32_t*)dstLFB)[x] = (R<<16)|(G<<8)|B;
    }
}

// Variante mit UNALIGNED Stores, falls vesa_pitch nicht durch 16 teilbar ist
// (sonst wuerde movntdq #GP -> EE=0D werfen). storeu ist minimal langsamer,
// aber sicher; wird nur genommen, wenn das Alignment es verlangt.
__attribute__((target("sse2")))
static void convert_row_sse_32_u(uint8_t* dstLFB,
                                 const uint8_t* y_row,
                                 const uint8_t* u_row,
                                 const uint8_t* v_row,
                                 int w) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i c128 = _mm_set1_epi16(128);
    int x = 0;
    for (; x + 8 <= w; x += 8) {
        __m128i Y8 = _mm_loadl_epi64((const __m128i*)(y_row + x));
        __m128i Y  = _mm_unpacklo_epi8(Y8, zero);
        __m128i U4 = _mm_cvtsi32_si128(*(const int*)(u_row + x/2));
        __m128i V4 = _mm_cvtsi32_si128(*(const int*)(v_row + x/2));
        U4 = _mm_unpacklo_epi8(U4, zero);
        V4 = _mm_unpacklo_epi8(V4, zero);
        __m128i U = _mm_sub_epi16(_mm_unpacklo_epi16(U4, U4), c128);
        __m128i V = _mm_sub_epi16(_mm_unpacklo_epi16(V4, V4), c128);
        __m128i V_R = _mm_add_epi16(V, _mm_add_epi16(_mm_add_epi16(shr_s16(V,2), shr_s16(V,3)), shr_s16(V,5)));
        __m128i U_B = _mm_add_epi16(U, _mm_add_epi16(_mm_add_epi16(shr_s16(U,1), shr_s16(U,2)), shr_s16(U,6)));
        __m128i U_G = _mm_add_epi16(_mm_add_epi16(shr_s16(U,1), shr_s16(U,4)), shr_s16(U,5));
        __m128i V_G = _mm_add_epi16(_mm_add_epi16(shr_s16(V,1), shr_s16(V,3)), _mm_add_epi16(shr_s16(V,4), shr_s16(V,5)));
        __m128i R = _mm_add_epi16(Y, V_R);
        __m128i B = _mm_add_epi16(Y, U_B);
        __m128i G = _mm_sub_epi16(Y, _mm_add_epi16(U_G, V_G));
        __m128i R8 = _mm_packus_epi16(R, R);
        __m128i G8 = _mm_packus_epi16(G, G);
        __m128i B8 = _mm_packus_epi16(B, B);
        __m128i BG = _mm_unpacklo_epi8(B8, G8);
        __m128i RA = _mm_unpacklo_epi8(R8, zero);
        __m128i px0 = _mm_unpacklo_epi16(BG, RA);
        __m128i px1 = _mm_unpackhi_epi16(BG, RA);
        _mm_storeu_si128((__m128i*)(dstLFB + (x + 0) * 4), px0);
        _mm_storeu_si128((__m128i*)(dstLFB + (x + 4) * 4), px1);
    }
    for (; x < w; x++) {
        int Y = y_row[x];
        int U = u_row[x/2] - 128;
        int V = v_row[x/2] - 128;
        int R = Y + (V + (V>>2) + (V>>3) + (V>>5));
        int G = Y - ((U>>1)+(U>>4)+(U>>5)) - ((V>>1)+(V>>3)+(V>>4)+(V>>5));
        int B = Y + (U + (U>>1) + (U>>2) + (U>>6));
        R = R<0?0:(R>255?255:R); G = G<0?0:(G>255?255:G); B = B<0?0:(B>255?255:B);
        ((uint32_t*)dstLFB)[x] = (R<<16)|(G<<8)|B;
    }
}
#endif

void vesa_draw_yuv420(uint32_t* lfb, 
                      const uint8_t* y_ptr, const uint8_t* u_ptr, const uint8_t* v_ptr,
                      int pic_width, int pic_height,
                      int stride_y, int stride_c,
                      int screen_width, int screen_height) 
{
    int w = pic_width < screen_width ? pic_width : screen_width;
    int h = pic_height < screen_height ? pic_height : screen_height;

#if USE_SSE_CONVERT
    // --- SSE-Single-Pass direkt in den LFB (nur 32bpp) ------------------
    if (vesa_bpp == 32) {
        // Sind alle Zeilenanfaenge 16-aligned? LFB-Basis ist gross aligned,
        // also haengt es nur an vesa_pitch. Wenn ja -> schnelle NT-Stores,
        // sonst -> sichere unaligned Stores (vermeidet EE=0D bei krummem pitch).
        int aligned = ((vesa_pitch & 15) == 0);
        uint64_t m0 = rdtsc64();
        for (int y = 0; y < h; y++) {
            uint8_t *dst_row = (uint8_t*)lfb + (y * vesa_pitch);
            const uint8_t *y_row = y_ptr + (y * stride_y);
            const uint8_t *u_row = u_ptr + ((y / 2) * stride_c);
            const uint8_t *v_row = v_ptr + ((y / 2) * stride_c);
            if (aligned) convert_row_sse_32  (dst_row, y_row, u_row, v_row, w);
            else         convert_row_sse_32_u(dst_row, y_row, u_row, v_row, w);
        }
        _mm_sfence();   // NT-Stores sichtbar machen
        uint64_t m1 = rdtsc64();
        static int sN = 0;
        if (sN++ < 5) printf("SSE draw %u kcyc (%d rows, %s)\n",
                             (uint32_t)((m1 - m0) / 1000), h, aligned ? "nt" : "u");
        return;
    }
#endif

    // --- Skalar-Fallback (24/16bpp oder USE_SSE_CONVERT=0) --------------
    static uint32_t* backbuffer = NULL;
    if (!backbuffer) {
        backbuffer = (uint32_t*)malloc(screen_width * screen_height * 4);
    }

    for (int y = 0; y < h; y++) {
        uint8_t *dst_row = (uint8_t*)backbuffer + (y * vesa_pitch);
        const uint8_t *y_row = y_ptr + (y * stride_y);
        const uint8_t *u_row = u_ptr + ((y / 2) * stride_c);
        const uint8_t *v_row = v_ptr + ((y / 2) * stride_c);

        for (int x = 0; x < w; x+=2) {
            int U = u_row[x / 2] - 128;
            int V = v_row[x / 2] - 128;
            int U_G = (U >> 1) + (U >> 4) + (U >> 5);
            int V_G = (V >> 1) + (V >> 3) + (V >> 4) + (V >> 5);
            int U_B = U + (U >> 1) + (U >> 2) + (U >> 6);
            int V_R = V + (V >> 2) + (V >> 3) + (V >> 5);

            for (int dx = 0; dx < 2; dx++) {
                int Y = y_row[x + dx];
                int R = Y + V_R;
                int G = Y - U_G - V_G;
                int B = Y + U_B;
                R = R < 0 ? 0 : (R > 255 ? 255 : R);
                G = G < 0 ? 0 : (G > 255 ? 255 : G);
                B = B < 0 ? 0 : (B > 255 ? 255 : B);

                if (vesa_bpp == 32) {
                    ((uint32_t*)dst_row)[x + dx] = (R << 16) | (G << 8) | B;
                } else if (vesa_bpp == 24) {
                    dst_row[(x+dx)*3 + 0] = B;
                    dst_row[(x+dx)*3 + 1] = G;
                    dst_row[(x+dx)*3 + 2] = R;
                } else if (vesa_bpp == 16) {
                    ((uint16_t*)dst_row)[x + dx] = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
                }
            }
        }
    }

    uint32_t blitBytes = (uint32_t)h * (uint32_t)vesa_pitch;
    uint64_t m0 = rdtsc64();
    memcpy_wc(lfb, backbuffer, blitBytes);
    uint64_t m1 = rdtsc64();
    static int mN = 0;
    if (mN++ < 5) printf("LFB memcpy_wc %u kcyc (%d rows)\n", (uint32_t)((m1 - m0) / 1000), h);
}
