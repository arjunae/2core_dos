#ifndef VESA_H
#define VESA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize VESA mode (e.g. 1024, 768)
// Returns the linear pointer to the frame buffer, or NULL on failure.
uint32_t* vesa_init(int width, int height);

// Dump all VBE modes matching a resolution (bpp, LFB, mode number)
void vesa_dump_modes(int want_w, int want_h);

// Restore normal DOS text mode
void vesa_restore_text_mode();

// Wait for VSYNC
void vesa_wait_vsync();

// Convert a YUV420 planar frame to RGB32 and write to the frame buffer
void vesa_draw_yuv420(uint32_t* lfb, 
                      const uint8_t* y_ptr, const uint8_t* u_ptr, const uint8_t* v_ptr,
                      int pic_width, int pic_height,
                      int stride_y, int stride_c,
                      int screen_width, int screen_height);

extern int vesa_pitch;
extern int vesa_bpp;
extern uint32_t vesa_phys_base;

#ifdef __cplusplus
}
#endif

#endif
