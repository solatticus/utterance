#ifndef FX_H
#define FX_H

typedef enum {
    FX_VIGNETTE = 0,
    FX_TUNNEL,
    FX_CHROMATIC,
    FX_SCANLINE,
    FX_OFF,
    FX_COUNT
} FxMode;

void   fx_init(int w, int h);
void   fx_resize(int w, int h);
void   fx_cycle(void);
FxMode fx_mode(void);
void   fx_begin(float progress);           /* progress > 0 = bind FBO */
void   fx_end(int w, int h, float progress); /* draw quad if progress > 0 */
void   fx_destroy(void);

#endif
