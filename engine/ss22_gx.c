/*
 * ss22_gx.c -- a Super System 22 frame on the Wii's GPU (GX). The PC renderer is engine/ss22_gl.c (OpenGL); this file is
 * its Wii counterpart behind the same interface (engine/ss22_gl.h), built instead of it by the Wii target.
 *
 * Order of a frame, as engine/ss22_gl.c draws it:
 *   1. the mixer's background colour
 *   2. the quads and the sprites in ONE far-to-near walk (MAME queues both into the same radix tree): each quad textured from
 *      the texture ROM through its palette group, shaded by its per-vertex brightness, CZ-fogged and poly-faded; each sprite
 *      rendered by the CPU (sprite_hw.c, the validated model) into its own bounding box and blended
 *   3. the screen fade over everything drawn so far
 *   4. the text layer (text_hw.c, re-rendered only when what it reads changes), then the sprites that sit over the text
 *      (prioverchar) once more, so the text does not cover them
 * The mixer's final gamma is not applied yet.
 *
 * How the PC pipeline maps onto GX:
 *   - texels: the texture ROM is 8-bit pens resolved through a 256-colour palette group, which is exactly GX's CI8 format
 *     with a TLUT. A quad's UV box is baked ONCE into CI8 (tex_bake.c's two-stage tilemap fetch, cmode sub-pens included)
 *     and cached by its box alone; the palette is applied by the GPU from a per-group TLUT, so a palette change costs one
 *     512-byte TLUT, never a re-bake.
 *   - perspective: every vertex goes out in EYE space (x*Z, y*Z, -Z) through a projection whose divide by Z puts it back on
 *     its exact screen pixel, so the GPU interpolates the texture perspective-correctly by itself.
 *   - shade bri/64 can exceed 1 (over-brightening): the vertex colour carries bri/4 and TEV stage 0 scales by 4, the same
 *     exact trick as quad_gl.c's GL_RGB_SCALE.
 *   - CZ fog: the vertex alpha is the fog factor (255 = none) and TEV stage 1 blends towards the fog colour.
 *   - the poly fade multiplies after the fog, so it is folded into the vertex colour and the fog colour.
 *
 * Where it differs from the PC, knowingly:
 *   - the palette is RGB565 in the TLUT (5-6 bits a channel), and the mixer's gamma is not applied;
 *   - a sprite over the text layer is drawn again over it, where the PC cuts the text out under the sprite's pixels: the same
 *     picture wherever the sprite is opaque (the HUD), a slightly different one under its translucent pixels;
 *   - a quad whose texture does not fit in the cache is baked at a lower resolution rather than not drawn.
 */
#include "ss22_gl.h"

#if defined(__WII__) || defined(__wii__)

#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eng.h"
#include "fog_hw.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "sprite_hw.h"
#include "text_hw.h"
#include "../timecris/wii/port.h"

#define SW 640
#define SH 480

/* ------------------------------------------------------------------ the frame's quads, compact */
/* A geo_quad is ~1 KB (v, rv and dv, ten vertices each) and a frame holds ~2000: only what the draw needs is kept. */
typedef struct {
    int32_t  sx16, sy16, z;
    uint16_t u, v;
    int32_t  bri;
} gq_vert;

typedef struct gx_tex gx_tex;

typedef struct {
    gq_vert  rv[10];
    int      nrv;
    int32_t  clip[4];
    uint16_t uvbox[4];
    uint32_t color;
    int32_t  zsort, cz_adjust;
    uint8_t  texbank, cmode, cz_type;
    int      order;
    gx_tex  *tex;                       /* resolved before the draw */
} gq;

typedef struct { int32_t zsort; int order; int idx; } gq_key;

static gq      *qbuf;
static gq_key  *qkeys;
static int      qn, qcap, qorder;

/* A fixed capacity, allocated once at start-up (gx_init_once): it once grew with realloc, which failed silently with MEM2 full
 * and dropped every quad of every frame (a frame with geometry time and 0 quads). The most seen in a frame is ~1600; each
 * slot is ~210 bytes, so the room left over goes to the texture cache. */
#define QMAX 2560
static int st_qdropped;                      /* quads past QMAX (statistics) */

static void push_quad(const geo_quad *q, void *user)
{
    (void)user;
    if (q->nrv < 3) return;
    if (qn >= qcap) { st_qdropped++; return; }
    gq *d = &qbuf[qn];
    d->nrv = q->nrv > 10 ? 10 : q->nrv;
    for (int i = 0; i < d->nrv; i++) {
        d->rv[i].sx16 = q->rv[i].sx16; d->rv[i].sy16 = q->rv[i].sy16; d->rv[i].z = q->rv[i].z;
        d->rv[i].u = (uint16_t)q->rv[i].u; d->rv[i].v = (uint16_t)q->rv[i].v; d->rv[i].bri = q->rv[i].bri;
    }
    memcpy(d->clip, q->clip, sizeof d->clip);
    memcpy(d->uvbox, q->uvbox, sizeof d->uvbox);
    d->color = q->color; d->zsort = q->zsort; d->cz_adjust = q->cz_adjust;
    d->texbank = (uint8_t)q->texbank; d->cmode = (uint8_t)q->cmode; d->cz_type = (uint8_t)q->cz_type;
    d->order = qorder++;
    d->tex = NULL;
    qn++;
}

/* quad_gl.c's eng_quad_sort: far (large zsort) first; ties draw in REVERSE submission order (MAME prepends to a radix
 * bucket, so the first quad emitted at a zsort ends up on top). The keys are sorted, the quads stay put. */
static int key_cmp(const void *a, const void *b)
{
    const gq_key *ka = a, *kb = b;
    if (ka->zsort != kb->zsort) return ka->zsort > kb->zsort ? -1 : 1;
    return ka->order > kb->order ? -1 : (ka->order < kb->order ? 1 : 0);
}

/* ------------------------------------------------------------------ the texture cache: CI8 bakes of a quad's UV box */
#define TEX_ENTRIES  4096                    /* 2048 filled up in gameplay with the byte budget still a fifth unused */
#define TEX_BUCKETS  4096
/* texels per axis; a wider box is decimated by an integer step (tex_bake.c). The first is the normal one; the others are
 * what a quad gets when the cache cannot fit it this frame (everything in it is drawn this frame), instead of nothing. */
static const int tex_caps[] = { 256, 64, 16 };

struct gx_tex {
    uint16_t min_u, min_v, range_u, range_v;   /* the box after tex_bake.c's 16-texel widening */
    uint8_t  texbank, cmode, step_u, step_v;
    uint16_t cap;
    uint16_t tw, th;                           /* storage: whole 8x4 CI8 tiles */
    uint32_t bytes;
    uint8_t *data;
    uint32_t frame;                            /* last frame that drew it: never evicted while the GPU may still need it */
    gx_tex  *hnext, *prev, *next;              /* bucket chain; LRU list (head = most recent) */
};

static gx_tex  *tex_pool;
static gx_tex  *tex_free_list;
static gx_tex  *tex_bucket[TEX_BUCKETS];
static gx_tex  *lru_head, *lru_tail;
static uint32_t tex_bytes, tex_budget;
static int      tex_count;

static uint32_t tex_hash(int min_u, int min_v, int range_u, int range_v, int texbank, int cmode, int cap)
{
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)min_u) * 16777619u;   h = (h ^ (uint32_t)min_v) * 16777619u;
    h = (h ^ (uint32_t)range_u) * 16777619u; h = (h ^ (uint32_t)range_v) * 16777619u;
    h = (h ^ (uint32_t)texbank) * 16777619u; h = (h ^ (uint32_t)cmode) * 16777619u;
    h = (h ^ (uint32_t)cap) * 16777619u;
    return h & (TEX_BUCKETS - 1);
}

static void lru_unlink(gx_tex *t)
{
    if (t->prev) t->prev->next = t->next; else lru_head = t->next;
    if (t->next) t->next->prev = t->prev; else lru_tail = t->prev;
    t->prev = t->next = NULL;
}

static void lru_front(gx_tex *t)
{
    t->prev = NULL; t->next = lru_head;
    if (lru_head) lru_head->prev = t; else lru_tail = t;
    lru_head = t;
}

/* Drop the least recently drawn texture. Every use moves an entry to the head, so the tail is the oldest: when it was drawn
 * this frame, so was every other one, and nothing can go (0). */
static int tex_evict_one(uint32_t frame)
{
    gx_tex *t = lru_tail;
    if (!t || t->frame == frame) return 0;
    const uint32_t b = tex_hash(t->min_u, t->min_v, t->range_u, t->range_v, t->texbank, t->cmode, t->cap);
    for (gx_tex **pp = &tex_bucket[b]; *pp; pp = &(*pp)->hnext)
        if (*pp == t) { *pp = t->hnext; break; }
    lru_unlink(t);
    free(t->data);
    tex_bytes -= t->bytes;
    tex_count--;
    t->data = NULL;
    t->hnext = tex_free_list; tex_free_list = t;
    return 1;
}

/* tex_bake.c's cmode decode: (palette offset, sub-pen shift, sub-pen mask) */
static void cmode_params(int cmode, int *offset, int *shift, int *mask)
{
    cmode &= 0xF;
    if (cmode & 4)      { *offset = 0xEC + ((cmode & 8) << 1); *mask = 0x03; *shift = 2 * (~cmode & 3); }
    else if (cmode & 2) { *offset = 0xE0 + ((cmode & 8) << 1); *mask = 0x0F; *shift = 4 * (~cmode & 1); }
    else                { *offset = 0; *mask = 0xFF; *shift = 0; }
}

static int tex_bakes, tex_hits, tex_fails, tex_lowres;

/* The CI8 texture of one (widened) UV box at one cap: cached, or baked now. NULL = it does not fit this frame. */
static gx_tex *tex_get(int min_u, int min_v, int range_u, int range_v, int texbank, int cmode, int cap, uint32_t frame)
{
    const uint32_t b = tex_hash(min_u, min_v, range_u, range_v, texbank, cmode, cap);
    for (gx_tex *t = tex_bucket[b]; t; t = t->hnext)
        if (t->min_u == min_u && t->min_v == min_v && t->range_u == range_u && t->range_v == range_v &&
            t->texbank == texbank && t->cmode == cmode && t->cap == cap) {
            t->frame = frame;
            lru_unlink(t); lru_front(t);
            tex_hits++;
            return t;
        }

    /* tex_bake.c's sizing: one baked texel per source texel up to the cap, an integer step over it, and ONE GUARD texel past
     * the used corner so a sample on the far edge reads the neighbouring source texel, not padding */
    int step_u = (range_u + cap - 1) / cap, step_v = (range_v + cap - 1) / cap;
    if (step_u < 1) step_u = 1;
    if (step_v < 1) step_v = 1;
    const int sw = (range_u + step_u - 1) / step_u, sh = (range_v + step_v - 1) / step_v;
    const int bw = sw + 1, bh = sh + 1;
    const int tw = (bw + 7) & ~7, th = (bh + 3) & ~3;   /* CI8 tiles are 8x4 texels */
    const uint32_t bytes = (uint32_t)tw * (uint32_t)th;

    while (tex_bytes + bytes > tex_budget || !tex_free_list)
        if (!tex_evict_one(frame)) return NULL;
    uint8_t *data;
    while (!(data = memalign(32, bytes)))
        if (!tex_evict_one(frame)) return NULL;

    memset(data, 0, bytes);
    int cm_offset, cm_shift, cm_mask;
    cmode_params(cmode, &cm_offset, &cm_shift, &cm_mask);
    const uint8_t *tmap = g_texture_tilemap, *tdata = g_texture_data;
    const int row_tiles = tw >> 3;
    for (int py = 0; py < bh; py++) {
        const int tv = min_v + py * step_v;
        const int v = (tv & 0xFFF) | (texbank * 0x1000);
        const int vrow = (v & 0xFFF0) << 4;              /* tilemap row base */
        const int local_y0 = v & 0xF;
        uint8_t *row = data + (size_t)(py >> 2) * row_tiles * 32 + (size_t)(py & 3) * 8;
        int last_idx = -1, attr = 0;
        uint32_t tile = 0;
        for (int px = 0; px < bw; px++) {
            uint8_t fetch = 0;
            if (tmap && tdata) {
                const int u = (min_u + px * step_u) & 0xFFF;
                const int tilemap_index = vrow | ((u & 0xFF0) >> 4);
                if (tilemap_index != last_idx) {         /* 16 consecutive u share one tilemap entry */
                    last_idx = tilemap_index;
                    const int byte_off = tilemap_index * 2;
                    if (byte_off + 1 < 0x200000) {
                        tile = tmap[byte_off] | (tmap[byte_off + 1] << 8);
                        const int ao = 0x200000 + (tilemap_index >> 1);
                        attr = ao < (int)TEXTUREMAP_SIZE ? ((tilemap_index & 1) ? (tmap[ao] & 0xF) : ((tmap[ao] >> 4) & 0xF)) : 0;   /* HIGH nibble on the even index */
                        if ((attr & 0x1) && !g_eng_tex_tile16) tile |= 0x10000;
                    } else { tile = 0; attr = 0; last_idx = -1; }
                }
                int local_x = u & 0xF, local_y = local_y0;
                if (attr & 0x4) local_x = 15 - local_x;  /* flips first, swap last (tex_bake.c) */
                if (attr & 0x2) local_y = 15 - local_y;
                if (attr & 0x8) { const int t = local_x; local_x = local_y; local_y = t; }
                const uint32_t po = tile * 256 + (uint32_t)local_y * 16 + (uint32_t)local_x;
                if (po < TEXTURE_TOTAL_SIZE) fetch = tdata[po];
            }
            row[(size_t)(px >> 3) * 32 + (px & 7)] = (uint8_t)((cm_offset + ((fetch >> cm_shift) & cm_mask)) & 0xFF);
        }
    }
    DCFlushRange(data, bytes);

    gx_tex *t = tex_free_list; tex_free_list = t->hnext;
    t->min_u = (uint16_t)min_u; t->min_v = (uint16_t)min_v; t->range_u = (uint16_t)range_u; t->range_v = (uint16_t)range_v;
    t->texbank = (uint8_t)texbank; t->cmode = (uint8_t)cmode; t->step_u = (uint8_t)step_u; t->step_v = (uint8_t)step_v;
    t->cap = (uint16_t)cap; t->tw = (uint16_t)tw; t->th = (uint16_t)th; t->bytes = bytes; t->data = data; t->frame = frame;
    t->hnext = tex_bucket[b]; tex_bucket[b] = t;
    lru_front(t);
    tex_bytes += bytes;
    tex_count++;
    tex_bakes++;
    return t;
}

/* the quad's texture at the best cap that fits this frame */
static gx_tex *tex_for(int min_u, int min_v, int range_u, int range_v, int texbank, int cmode, uint32_t frame)
{
    for (size_t i = 0; i < sizeof tex_caps / sizeof tex_caps[0]; i++) {
        gx_tex *t = tex_get(min_u, min_v, range_u, range_v, texbank, cmode, tex_caps[i], frame);
        if (t) { if (i) tex_lowres++; return t; }
    }
    tex_fails++;
    return NULL;
}

/* ------------------------------------------------------------------ palettes: one RGB565 TLUT per group, 16 in TMEM */
static uint16_t (*tluts)[ENG_PAL_ENTRIES];
static uint16_t tlut_gen[ENG_PAL_GROUPS];
static uint8_t  tlut_built[ENG_PAL_GROUPS];
static int      tmem_group[16];
static uint32_t tmem_stamp[16], tmem_clock;
static int      tlut_loads;

static void tlut_refresh(int g)
{
    if (tlut_built[g] && tlut_gen[g] == g_eng_pal_gen[g]) return;
    for (int p = 0; p < ENG_PAL_ENTRIES; p++) {
        const uint8_t *c = direct_palette[g][p];
        tluts[g][p] = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
    }
    DCFlushRange(tluts[g], sizeof tluts[g]);
    tlut_built[g] = 1;
    tlut_gen[g] = g_eng_pal_gen[g];
    for (int s = 0; s < 16; s++) if (tmem_group[s] == g) tmem_group[s] = -1;   /* the copy in TMEM is stale */
}

/* The TMEM slot holding group g, loading it into the least recently used slot if none does. A load goes through the FIFO in
 * order, so quads already queued keep the colours they were queued with. */
static int tlut_slot(int g)
{
    int lru = 0;
    for (int s = 0; s < 16; s++) {
        if (tmem_group[s] == g) { tmem_stamp[s] = ++tmem_clock; return s; }
        if (tmem_stamp[s] < tmem_stamp[lru]) lru = s;
    }
    GXTlutObj to;
    GX_InitTlutObj(&to, tluts[g], GX_TL_RGB565, ENG_PAL_ENTRIES);
    GX_LoadTlut(&to, GX_TLUT0 + lru);
    tmem_group[lru] = g;
    tmem_stamp[lru] = ++tmem_clock;
    tlut_loads++;
    return lru;
}

/* ------------------------------------------------------------------ screen-space clip (quad_gl.c's clip_to_screen) */
/* geo_hw's near-plane clip puts its new vertices at z=1, which project far off-screen: the polygon is cut against the
 * screen rectangle first, its attributes interpolated linearly in SCREEN space (right for s=u/z, t=v/z, w=1/z and shade). */
typedef struct { float x, y, s, t, w, bri; } sv_t;

static int clip_edge(const sv_t *in, int n, sv_t *out, int axis, float limit, int keep_greater)
{
    int m = 0;
    for (int i = 0; i < n && m < 30; i++) {
        const sv_t *a = &in[i], *b = &in[(i + 1) % n];
        const float av = axis ? a->y : a->x, bv = axis ? b->y : b->x;
        const int ain = keep_greater ? (av >= limit) : (av <= limit);
        const int bin = keep_greater ? (bv >= limit) : (bv <= limit);
        if (ain) out[m++] = *a;
        if (ain != bin && m < 30) {
            const float d = bv - av, t = d != 0.0f ? (limit - av) / d : 0.0f;
            sv_t v;
            v.x = a->x + (b->x - a->x) * t;  v.y = a->y + (b->y - a->y) * t;
            v.s = a->s + (b->s - a->s) * t;  v.t = a->t + (b->t - a->t) * t;
            v.w = a->w + (b->w - a->w) * t;  v.bri = a->bri + (b->bri - a->bri) * t;
            out[m++] = v;
        }
    }
    return m;
}

static int clip_to_screen(const sv_t *in, int n, sv_t *out)
{
    sv_t a[32], b[32];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) a[i] = in[i];
    n = clip_edge(a, n, b, 0, 0.0f, 1);       if (n < 3) return 0;
    n = clip_edge(b, n, a, 0, (float)SW, 0);  if (n < 3) return 0;
    n = clip_edge(a, n, b, 1, 0.0f, 1);       if (n < 3) return 0;
    n = clip_edge(b, n, a, 1, (float)SH, 0);  if (n < 3) return 0;
    for (int i = 0; i < n; i++) out[i] = a[i];
    return n;
}

/* ------------------------------------------------------------------ RGBA -> RGB5A3 (GX's 4x4 tiles) */
static inline uint16_t rgb5a3(const uint8_t *p)
{
    const unsigned a = p[3];
    return a >= 0xE0 ? (uint16_t)(0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3))
                     : (uint16_t)(((a >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
}

/* w x h RGBA (row-major) into a tw x th RGB5A3 texture (tw, th multiples of 4; the padding is transparent). Returns whether
 * any pixel is visible. */
static bool to_rgb5a3(const uint8_t *src, int w, int h, uint16_t *dst, int tw, int th)
{
    unsigned any = 0;
    uint16_t *o = dst;
    for (int by = 0; by < th; by += 4)
        for (int bx = 0; bx < tw; bx += 4)
            for (int y = by; y < by + 4; y++) {
                if (y >= h) { o[0] = o[1] = o[2] = o[3] = 0; o += 4; continue; }
                const uint8_t *p = src + ((size_t)y * w + (size_t)bx) * 4;
                for (int x = bx; x < bx + 4; x++, p += 4) {
                    if (x >= w) { *o++ = 0; continue; }
                    any |= p[3];
                    *o++ = rgb5a3(p);
                }
            }
    DCFlushRange(dst, (size_t)tw * th * 2);
    return any != 0;
}

/* ------------------------------------------------------------------ the sprites: one texture each, from a per-frame arena */
#define SPR_ITEMS 1024
#define SPR_ARENA (640u << 10)               /* a frame's sprite textures: 640x480 of them at 2 bytes, with room to spare */
/* THE KEEP: two arenas, this frame's and last frame's. A sprite whose key (sprite_item_key: every input of its pixels) was
 * converted last frame is copied over from the other arena instead of rendered again -- a transition's full-screen
 * banners stand still for many frames. Last frame's arena is free to read: the present waited for the GPU. */
#define SPR_KEEP 2048                        /* open addressing, a power of two over SPR_ITEMS */
typedef struct { uint64_t key; uint16_t *tex; int tw, th; bool used; } spr_keep;
static spr_keep *spr_keep_tab[2];             /* SPR_KEEP each, allocated with the arenas (not in MEM1's static image) */
static uint8_t *spr_arenas[2];
static int      spr_cur;
static int      spr_hits;                    /* sprites reused from last frame (statistics) */

typedef struct {
    uint16_t *tex;                           /* NULL = not drawn (nothing visible, or no room) */
    int       tw, th;
} spr_tex;

static text_state   tst;
static sprite_state sst;
static bool         have_text, have_spr;
static sprite_item  items[SPR_ITEMS];
static spr_tex      itex[SPR_ITEMS];
static int          ni;
static uint8_t     *spr_arena;
static uint32_t     spr_arena_used;
static uint8_t     *spr_scratch;             /* one sprite's RGBA, grown to the largest seen */
static size_t       spr_scratch_cap;
static int          spr_dropped;
static long         st_spr_px;               /* sprite bounding-box pixels rendered (statistics) */

static spr_keep *keep_slot(spr_keep *tab, uint64_t key)
{
    uint32_t s = (uint32_t)(key ^ (key >> 32)) & (SPR_KEEP - 1);
    while (tab[s].used && tab[s].key != key) s = (s + 1) & (SPR_KEEP - 1);
    return &tab[s];
}

static void sprites_render(void)
{
    spr_cur ^= 1;
    spr_arena = spr_arenas[spr_cur];
    spr_keep *now = spr_keep_tab[spr_cur], *last = spr_keep_tab[spr_cur ^ 1];
    memset(now, 0, sizeof *now * SPR_KEEP);
    spr_arena_used = 0;
    for (int i = 0; i < ni; i++) {
        const sprite_item *it = &items[i];
        itex[i].tex = NULL;
        if (it->w <= 0 || it->h <= 0) continue;
        const int tw = (it->w + 3) & ~3, th = (it->h + 3) & ~3;
        const uint32_t bytes = ((uint32_t)tw * th * 2 + 31) & ~31u;
        const uint64_t key = sprite_item_key(&sst, &g_fog, it);
        spr_keep *k = keep_slot(now, key);
        if (k->used) {                                   /* the same sprite twice this frame: share the texture */
            if (k->tex) { itex[i].tex = k->tex; itex[i].tw = k->tw; itex[i].th = k->th; }
            continue;
        }
        const spr_keep *old = keep_slot(last, key);
        if (old->used && !old->tex) {                    /* nothing visible last frame either */
            k->used = true; k->key = key; k->tex = NULL;
            continue;
        }
        if (spr_arena_used + bytes > SPR_ARENA) { spr_dropped++; continue; }
        uint16_t *dst = (uint16_t *)(spr_arena + spr_arena_used);
        if (old->used) {                                 /* converted last frame: copy it across */
            memcpy(dst, old->tex, bytes);
            DCFlushRange(dst, bytes);
            spr_hits++;
        } else {
            const size_t need = (size_t)it->w * it->h * 4;
            if (need > spr_scratch_cap) {
                uint8_t *nb = realloc(spr_scratch, need);
                if (!nb) { spr_dropped++; continue; }
                spr_scratch = nb; spr_scratch_cap = need;
            }
            sprite_render_item(&sst, &g_fog, it, spr_scratch, 1);
            st_spr_px += (long)it->w * it->h;
            if (!to_rgb5a3(spr_scratch, it->w, it->h, dst, tw, th)) {
                k->used = true; k->key = key; k->tex = NULL;
                continue;
            }
        }
        itex[i].tex = dst; itex[i].tw = tw; itex[i].th = th;
        k->used = true; k->key = key; k->tex = dst; k->tw = tw; k->th = th;
        spr_arena_used += bytes;
    }
}

/* ------------------------------------------------------------------ the text layer, cached */
static uint8_t  *cgbuf;                      /* CGRAM with the text RAM as its tail: the text model's layout (ss22_gl.c) */
static const uint8_t *txt_pal;
static const uint16_t *txt_spot;
static bool      txt_spot_on;
static uint8_t  *txt_rgba;
static uint16_t *txt_tex;                    /* RGB5A3, 640x480 */
static bool      txt_visible;
static uint32_t  txt_global;                 /* everything a text pixel reads that is not its own tile */
static bool      txt_have;
static int       txt_renders, txt_cells;     /* full redraws, and cells redrawn alone */
static uint32_t  txt_part[5];                /* last frame's global inputs: palette, scroll, mixer registers, spot, fade */
static fog_state txt_fog;                    /* g_fog as the layer is drawn with: no screen fade when the GPU applies it */
static int       txt_fade_gpu, txt_fade_f;   /* the TEV applies the fade / its factor this frame (0 = none) */
static uint8_t   txt_fade_rgb[3];
static int       txt_why[5];                 /* full redraws each of them caused (statistics) */

/* the layer kept between frames in 16x16 screen cells, each with the signature of the tiles it shows */
#define TCW (SW / 16)
#define TCH (SH / 16)
static uint32_t  cell_key[TCW * TCH];
static uint8_t   cell_vis[TCW * TCH];
static int       vis_cells;
static uint32_t  chash[1024], cstamp[1024], cframe;   /* a character's hash, computed once a frame when a tile needs it */

static uint32_t hash_words(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) { uint32_t w; memcpy(&w, b + i, 4); h = (h ^ w) * 16777619u; }
    for (; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* tilemap tile (col, row) as a pixel of it reads it: its word in the text RAM and its character's 128 bytes */
static uint32_t char_hash(int code)
{
    if (cstamp[code] != cframe) { cstamp[code] = cframe; chash[code] = hash_words(2166136261u, cgbuf + (size_t)code * 128, 128); }
    return chash[code];
}

static uint32_t tile_key(int col, int row)
{
    const uint8_t *w = cgbuf + 0x1E000 + ((size_t)row * 64 + (size_t)col) * 2;
    const uint32_t d = (uint32_t)((w[0] << 8) | w[1]);
    return (d * 2654435761u) ^ char_hash((int)(d & 0x3FF));
}

/* screen cell (cx, cy) of the RGBA layer into its 4x4 RGB5A3 blocks of the texture; whether a pixel of it is visible */
static bool convert_cell(int cx, int cy)
{
    unsigned any = 0;
    for (int by = 0; by < 4; by++) {
        const int y0 = cy * 16 + by * 4;
        uint16_t *row = txt_tex + ((size_t)(y0 >> 2) * (SW >> 2) + (size_t)cx * 4) * 16;   /* the cell's 4 blocks on this row, contiguous */
        uint16_t *o = row;
        for (int bx = 0; bx < 4; bx++)
            for (int y = 0; y < 4; y++) {
                const uint8_t *p = txt_rgba + ((size_t)(y0 + y) * SW + (size_t)(cx * 16 + bx * 4)) * 4;
                for (int x = 0; x < 4; x++, p += 4) { any |= p[3]; *o++ = rgb5a3(p); }
            }
        DCFlushRange(row, 4 * 16 * 2);
    }
    return any != 0;
}

/* text_render is a pure function of what it reads, and a pixel reads only its own tile plus what is global to the layer
 * (the scroll, the mixer's text registers, the spot table, and the 256 palette entries at the text's palette base -- every
 * pen it can draw). A global change redraws the layer; otherwise only the 16x16 cells whose tiles changed are redrawn and
 * re-converted, which in a game is the few digits of the clock and the score. */
static void text_update(void)
{
    /* the global inputs in five groups, hashed apart so the log can say which one forced a full redraw */
    const int palbase = (g_fog.text_palbase << 8) & 0x7F00;
    uint32_t part[5];
    part[0] = 2166136261u;
    for (int plane = 0; plane < 3; plane++) part[0] = hash_words(part[0], txt_pal + plane * 0x8000 + palbase, 256);
    part[1] = hash_words(2166136261u, tst.attr, sizeof tst.attr);
    const uint8_t regs[8] = { g_fog.mixer_flags, g_fog.text_palbase, g_fog.text_alpha, g_fog.text_alpha_lo,
                              g_fog.text_alpha_hi, g_fog.text_alpha_mask, (uint8_t)txt_spot_on, 0 };
    part[2] = hash_words(2166136261u, regs, sizeof regs);
    part[3] = hash_words(2166136261u, &g_fog.spot_factor, sizeof g_fog.spot_factor);
    if (txt_spot_on && txt_spot) part[3] = hash_words(part[3], txt_spot, 0x1000);
    const int fade_en = ((g_fog.mixer_flags & 2) != 0) && g_fog.screen_fade_factor;   /* text_hw.c gates the fade the same way */
    /* THE FADE ON THE GPU: the screen fade moves every pixel of the layer towards one colour by one factor -- a transition
     * steps it each frame, which redrew the whole layer each frame (58 of 59 full redraws in the attract). The layer is kept
     * unfaded and the TEV applies the fade as it draws it (gx_text_layer). Not with the spot table on: its darkening pens
     * are left unfaded by text_hw.c, which one TEV blend over the layer cannot tell apart. */
    txt_fade_gpu = !txt_spot_on;
    txt_fade_f = (txt_fade_gpu && fade_en) ? g_fog.screen_fade_factor : 0;
    memcpy(txt_fade_rgb, g_fog.screen_fade, 3);
    txt_fog = g_fog;
    if (txt_fade_gpu) txt_fog.screen_fade_factor = 0;      /* text_hw.c draws no fade */
    part[4] = hash_words(2166136261u, &txt_fade_gpu, sizeof txt_fade_gpu);
    if (!txt_fade_gpu) {
        part[4] = hash_words(part[4], &fade_en, sizeof fade_en);
        if (fade_en) { part[4] = hash_words(part[4], g_fog.screen_fade, 3); part[4] = hash_words(part[4], &g_fog.screen_fade_factor, 1); }
    }
    uint32_t h = 2166136261u;
    for (int i = 0; i < 5; i++) h = (h ^ part[i]) * 16777619u;

    const bool full = !txt_have || h != txt_global;
    if (full && txt_have)
        for (int i = 0; i < 5; i++) if (part[i] != txt_part[i]) txt_why[i]++;
    memcpy(txt_part, part, sizeof part);
    if (full) {
        text_render(&tst, &txt_fog, txt_rgba, 0);
        txt_global = h; txt_have = true;
        txt_renders++;
    }
    cframe++;
    const int sx = (tst.attr[0] - 0x35C) & 0x3FF, sy = tst.attr[1] & 0x3FF;   /* text_hw.c's scroll */
    for (int cy = 0; cy < TCH; cy++) {
        const int ta = ((cy * 16 + sy) & 0x3FF) >> 4, tb = ((cy * 16 + 15 + sy) & 0x3FF) >> 4;   /* the tile rows it shows */
        for (int cx = 0; cx < TCW; cx++) {
            const int ca = ((cx * 16 + sx) & 0x3FF) >> 4, cb = ((cx * 16 + 15 + sx) & 0x3FF) >> 4;
            uint32_t k = tile_key(ca, ta);
            if (cb != ca) k = k * 16777619u ^ tile_key(cb, ta);
            if (tb != ta) { k = k * 16777619u ^ tile_key(ca, tb); if (cb != ca) k = k * 16777619u ^ tile_key(cb, tb); }
            const int c = cy * TCW + cx;
            if (!full && k == cell_key[c]) continue;
            if (!full) { text_render_rect(&tst, &txt_fog, txt_rgba, cx * 16, cy * 16, 16, 16); txt_cells++; }
            cell_key[c] = k;
            const uint8_t vis = convert_cell(cx, cy);
            vis_cells += (int)vis - (int)cell_vis[c];
            cell_vis[c] = vis;
        }
    }
    txt_visible = vis_cells > 0;
}

/* ------------------------------------------------------------------ setup */
static bool ready, failed;
static uint32_t frame_no;

/* 32-aligned, never freed: MEM1's leftover if it fits, else the heap */
static void *big_alloc(size_t size)
{
    void *p = wii_port_mem1_alloc(size, 32);
    return p ? p : memalign(32, size);
}

static bool gx_init_once(void)
{
    if (ready) return true;
    if (failed || !wii_port_gx_ready()) return false;   /* GX is set up by the host (wii_port_configure) */
    /* The big, never-freed buffers first, from what MEM1 has left (wii_port_mem1_alloc: the heap leaves it behind once it
     * moves to MEM2), the most needed first; whatever does not fit there comes from the heap. MEM2 keeps its room for the
     * texture cache. */
    const uint32_t free1_before = (uint32_t)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());
    qbuf = big_alloc((size_t)QMAX * sizeof *qbuf);
    qkeys = big_alloc((size_t)QMAX * sizeof *qkeys);
    qcap = (qbuf && qkeys) ? QMAX : 0;
    txt_tex = big_alloc((size_t)SW * SH * 2);
    spr_arenas[0] = big_alloc(SPR_ARENA);
    spr_arenas[1] = big_alloc(SPR_ARENA);
    txt_rgba = big_alloc((size_t)SW * SH * 4);
    const uint32_t free1_after = (uint32_t)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());
    tex_pool = calloc(TEX_ENTRIES, sizeof *tex_pool);
    tluts = memalign(32, sizeof(uint16_t) * ENG_PAL_GROUPS * ENG_PAL_ENTRIES);
    cgbuf = malloc(0x20000);
    spr_arena = spr_arenas[0];
    spr_keep_tab[0] = calloc(SPR_KEEP, sizeof(spr_keep));
    spr_keep_tab[1] = calloc(SPR_KEEP, sizeof(spr_keep));
    wii_port_log("[WII-GX] %u KB of the renderer's buffers from MEM1's leftover (%u KB were free there)",
                 (unsigned)((free1_before - free1_after) >> 10), (unsigned)(free1_before >> 10));
    if (!qcap || !tex_pool || !tluts || !cgbuf || !txt_rgba || !txt_tex || !spr_arenas[0] || !spr_arenas[1] ||
        !spr_keep_tab[0] || !spr_keep_tab[1]) {
        wii_port_log("[WII-GX] cannot allocate the renderer's buffers");
        failed = true;
        return false;
    }
    for (int i = 0; i < TEX_ENTRIES; i++) { tex_pool[i].hnext = tex_free_list; tex_free_list = &tex_pool[i]; }
    for (int s = 0; s < 16; s++) tmem_group[s] = -1;

    /* the texture budget: what both arenas have left after everything else, less a margin for the run, 512 KB .. 6 MB. The
     * bakes come from the heap, which now (the ROM regions taken from the arenas directly, eng_alloc_big) grows in MEM1 first
     * and then in MEM2 -- counting MEM2 alone left the cache 1.8 MB and whole models flickering out. */
    {
        const uint32_t free1 = (uint32_t)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());
        const uint32_t free2 = (uint32_t)((uintptr_t)SYS_GetArena2Hi() - (uintptr_t)SYS_GetArena2Lo());
        const uint32_t freeh = (uint32_t)mallinfo().fordblks;   /* free inside the heap already (the boot's freed buffers) */
        const uint32_t margin = 1u << 20;
        const uint32_t all = free1 + free2 + freeh;
        uint32_t b = all > margin ? all - margin : 0;
        if (b > (6u << 20)) b = 6u << 20;
        if (b < (512u << 10)) b = 512u << 10;
        tex_budget = b;
        wii_port_log("[WII-GX] renderer up: texture budget %u KB (free: MEM1 %u KB, MEM2 %u KB, inside the heap %u KB)",
                     (unsigned)(b >> 10), (unsigned)(free1 >> 10), (unsigned)(free2 >> 10), (unsigned)(freeh >> 10));
    }

    /* three vertex formats: 0 = textured quad (position, colour, texcoord), 1 = layer / sprite (position, texcoord), 2 = flat */
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    ready = true;
    return true;
}

static void gx_common_state(void)
{
    GX_SetViewport(0.0f, 0.0f, (float)SW, (float)SH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, SW, SH);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);           /* the painter's order, as the hardware (no depth buffer) */
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    Mtx id;
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_InvalidateTexAll();                                 /* bakes and layers reuse memory: TMEM caches by address */
}

/* The draw state the next primitive needs; switching costs a few register writes, so it is done only on a change. */
enum { MODE_NONE, MODE_FLAT, MODE_FADE, MODE_TEX2D, MODE_QUADS };
static int gx_mode;

static void gx_ortho(void)
{
    Mtx44 p;
    guOrtho(p, 0.0f, (float)SH, 0.0f, (float)SW, 0.0f, 1.0f);
    GX_LoadProjectionMtx(p, GX_ORTHOGRAPHIC);
}

/* flat colour: the background (opaque) or the screen fade (blended) */
static void gx_mode_flat(bool blend)
{
    const int want = blend ? MODE_FADE : MODE_FLAT;
    if (gx_mode == want) return;
    gx_mode = want;
    gx_ortho();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    if (blend) GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    else       GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

static void gx_rect(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    GX_Begin(GX_QUADS, GX_VTXFMT2, 4);
    GX_Position3f32(0.0f, 0.0f, -0.5f);           GX_Color4u8(r, g, b, a);
    GX_Position3f32((float)SW, 0.0f, -0.5f);      GX_Color4u8(r, g, b, a);
    GX_Position3f32((float)SW, (float)SH, -0.5f); GX_Color4u8(r, g, b, a);
    GX_Position3f32(0.0f, (float)SH, -0.5f);      GX_Color4u8(r, g, b, a);
    GX_End();
}

static void gx_box(float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b)
{
    GX_Begin(GX_QUADS, GX_VTXFMT2, 4);
    GX_Position3f32(x0, y0, -0.5f); GX_Color4u8(r, g, b, 255);
    GX_Position3f32(x1, y0, -0.5f); GX_Color4u8(r, g, b, 255);
    GX_Position3f32(x1, y1, -0.5f); GX_Color4u8(r, g, b, 255);
    GX_Position3f32(x0, y1, -0.5f); GX_Color4u8(r, g, b, 255);
    GX_End();
}

/* The gun's crosshair, over everything, where the Wiimote points (the game gets the same position, engine/ss22_input_wii.c):
 * the PC's cross (engine/ss22_host.c), four red arms with a gap in the middle on a dark outline. None while it points off the
 * screen. */
bool ss22_input_aim(float *nx, float *ny);
static void gx_crosshair(void)
{
    float ax, ay;
    if (!ss22_input_aim(&ax, &ay)) return;
    const float cx = ax * (float)SW, cy = ay * (float)SH, s = (float)SH / 24.0f, g = s * 0.3f;
    gx_mode_flat(false);
    for (int pass = 0; pass < 2; pass++) {
        const float w = pass ? 1.0f : 2.0f, e = pass ? 0.0f : 1.0f;    /* half thickness; the outline also reaches 1 px further */
        const uint8_t r = pass ? 255 : 0, gb = pass ? 26 : 0;
        gx_box(cx - s - e, cy - w, cx - g + e, cy + w, r, gb, gb);
        gx_box(cx + g - e, cy - w, cx + s + e, cy + w, r, gb, gb);
        gx_box(cx - w, cy - s - e, cx + w, cy - g + e, r, gb, gb);
        gx_box(cx - w, cy + g - e, cx + w, cy + s + e, r, gb, gb);
    }
}

/* an RGB5A3 picture (a sprite, the text layer) blended over the frame at (x, y), w x h of its tw x th texels */
static void gx_mode_tex2d(void)
{
    if (gx_mode == MODE_TEX2D) return;
    gx_mode = MODE_TEX2D;
    gx_ortho();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

static void gx_picture(uint16_t *tex, int tw, int th, int x, int y, int w, int h)
{
    gx_mode_tex2d();
    GXTexObj to;
    GX_InitTexObj(&to, tex, (u16)tw, (u16)th, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&to, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&to, GX_TEXMAP0);
    const float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    const float su = (float)w / tw, sv = (float)h / th;
    GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
    GX_Position3f32(x0, y0, -0.5f); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x1, y0, -0.5f); GX_TexCoord2f32(su, 0.0f);
    GX_Position3f32(x1, y1, -0.5f); GX_TexCoord2f32(su, sv);
    GX_Position3f32(x0, y1, -0.5f); GX_TexCoord2f32(0.0f, sv);
    GX_End();
}

/* The text layer, with the screen fade applied here when text_update left it out: text_hw.c's
 * (c * (0xFF - f) + fade * f) / 0xFF is the TEV's lerp from the texel to C1 by the constant f, the alpha passed through. */
static void gx_text_layer(void)
{
    if (!txt_fade_f) { gx_picture(txt_tex, SW, SH, 0, 0, SW, SH); return; }
    gx_mode_tex2d();
    GX_SetTevColor(GX_TEVREG1, (GXColor){ txt_fade_rgb[0], txt_fade_rgb[1], txt_fade_rgb[2], 255 });
    GX_SetTevKColor(GX_KCOLOR0, (GXColor){ (u8)txt_fade_f, (u8)txt_fade_f, (u8)txt_fade_f, (u8)txt_fade_f });
    GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_C1, GX_CC_KONST, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    gx_picture(txt_tex, SW, SH, 0, 0, SW, SH);
    gx_mode = MODE_NONE;                                   /* the next 2D draw sets its plain REPLACE stage again */
}

/* the quads: eye-space vertices, two TEV stages (texel x shade x4, then the fog) */
static GXColor quads_fog;

static void gx_mode_quads(void)
{
    if (gx_mode == MODE_QUADS) return;
    gx_mode = MODE_QUADS;
    /* eye (X, Y, Zeye) = (x*Z, y*Z, -Z); the hardware divides by w = -Zeye = Z:
     *   ndc x = (2/640 * X + 1 * Zeye) / Z = x/320 - 1,  ndc y = (-2/480 * Y - 1 * Zeye) / Z = 1 - y/240,  ndc z = -0.5 */
    Mtx44 p;
    memset(p, 0, sizeof p);
    p[0][0] = 2.0f / SW;  p[0][2] = 1.0f;
    p[1][1] = -2.0f / SH; p[1][2] = -1.0f;
    p[2][2] = 0.5f;       p[2][3] = 0.0f;
    p[3][2] = -1.0f;
    GX_LoadProjectionMtx(p, GX_PERSPECTIVE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(2);
    /* stage 0: colour = texel * (bri/4) * 4, clamped; alpha = the vertex's fog factor */
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_4, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    /* stage 1: blend(prev, fog, a) = (1 - a) * fog + a * prev */
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_C0, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevColor(GX_TEVREG0, quads_fog);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

/* ------------------------------------------------------------------ the gun flash (ss22_gl.c's filter, no environment) */
static bool gun_flash = true;
void ss22_gl_set_gun_flash(bool on) { gun_flash = on; }
static void gun_flash_filter(void)
{
    static int prev_fade, run;
    const int fade = (g_fog.mixer_flags & 3) && g_fog.screen_fade_factor;
    const int white = fade && g_fog.screen_fade_factor >= 0xF0 && g_fog.screen_fade[0] >= 0xF0 && g_fog.screen_fade[1] >= 0xF0 &&
                      g_fog.screen_fade[2] >= 0xF0;
    if (!white) run = 0;
    else if (run || !prev_fade) run++;
    prev_fade = fade;
    if (!gun_flash && run && run <= 2) g_fog.screen_fade_factor = 0;
}

void ss22_gl_set_hud(const eng_hud_cfg *h) { (void)h; }   /* widescreen only: the Wii picture is 4:3 */

/* ------------------------------------------------------------------ per-frame statistics, into the boot log */
#define STAT_EVERY 120
static uint64_t st_prep, st_walk, st_tex, st_spr, st_txt, st_gx;
static int      st_quads, st_drawn, st_sprites, st_nowalk;
static int      quads_last, sprites_last;

static void stats_frame(void)
{
    if (++frame_no % STAT_EVERY) return;
    const double k = 1.0 / (STAT_EVERY * 1000.0);
    wii_port_log("[WII-GX] frame %u: quads %d drawn %d sprites %d (%ld px new, %d reused) (per frame) | tex: %d baked %d hits %d lowres "
                 "%d failed, %d cached, %u KB | tlut loads %d | text: %d full (palette %d scroll %d mixer %d spot %d fade %d), %d cells "
                 "| sprites dropped %d quads dropped %d | ms prep %.1f "
                 "(geometry %.1f) tex %.1f 2d %.1f (sprites %.1f text %.1f) gx %.1f | no-walk updates %d",
                 (unsigned)frame_no, st_quads / STAT_EVERY, st_drawn / STAT_EVERY, st_sprites / STAT_EVERY,
                 st_spr_px / STAT_EVERY, spr_hits / STAT_EVERY, tex_bakes, tex_hits, tex_lowres, tex_fails, tex_count, (unsigned)(tex_bytes >> 10),
                 tlut_loads, txt_renders, txt_why[0], txt_why[1], txt_why[2], txt_why[3], txt_why[4], txt_cells, spr_dropped, st_qdropped, st_prep * k, st_walk * k, st_tex * k, (st_spr + st_txt) * k, st_spr * k,
                 st_txt * k, st_gx * k, st_nowalk);
    st_prep = st_walk = st_tex = st_spr = st_txt = st_gx = 0;
    st_spr_px = 0; spr_hits = 0; st_qdropped = 0;
    memset(txt_why, 0, sizeof txt_why);
    st_quads = st_drawn = st_sprites = st_nowalk = 0;
    tex_bakes = tex_hits = tex_lowres = tex_fails = tlut_loads = txt_renders = txt_cells = spr_dropped = 0;
}

/* ------------------------------------------------------------------ the interface */
void ss22_prepare(const ss22_regs *r)
{
    const uint64_t t0 = wii_port_usec();
    g_eng_frame++;
    if (!r || !gx_init_once()) { qn = 0; ni = 0; have_text = have_spr = false; return; }

    fog_load_regs(r->mixer, r->czattr, r->czram);
    gun_flash_filter();
    eng_palette_from_planar(r->pal, 0x8000);

    text_set_spot(r->spotram, r->spot_enabled);
    txt_spot = r->spotram; txt_spot_on = r->spotram && r->spot_enabled;
    txt_pal = r->pal;
    memcpy(cgbuf, r->cgram, 0x1E000);                 /* the text model reads its text RAM at cg + 0x1E000 */
    memcpy(cgbuf + 0x1E000, r->textram, 0x2000);
    have_text = text_load_regs(&tst, cgbuf, r->pal, r->tilemapattr) != 0;

    ni = 0;
    have_spr = g_sprite_tiles && r->spriteram &&
               sprite_load_regs(&sst, r->spriteram, r->spriteram_size, r->vics, r->vics_size, r->vics_ctl, r->pal) != 0;
    if (have_spr) ni = sprite_collect(&sst, &g_fog, items, SPR_ITEMS);

    /* The list is walked every update, as the first Wii pass did: the frame rule (r->walk) is counted, not obeyed, until it
     * is known to hold on this backend -- obeying it shows no 3D on the updates it rejects. */
    if (!r->walk) st_nowalk++;
    qn = 0; qorder = 0;
    const uint64_t tw = wii_port_usec();
    if (r->poly_word) {
        const eng_list_cfg cfg = { ENG_LIST_HEAD_SS22, 0, NULL, NULL, NULL, NULL };
        eng_walk_list(r->poly_word, &cfg, push_quad, NULL);
    }
    st_walk += wii_port_usec() - tw;
    for (int i = 0; i < qn; i++) { qkeys[i].zsort = qbuf[i].zsort; qkeys[i].order = qbuf[i].order; qkeys[i].idx = i; }
    if (qn > 1) qsort(qkeys, (size_t)qn, sizeof *qkeys, key_cmp);
    quads_last = qn;
    sprites_last = ni;
    st_quads += qn;
    st_sprites += ni;
    st_prep += wii_port_usec() - t0;
}

/* one quad, in its turn */
static gx_tex *bound;
static int     bound_slot, sc[4], drawn;
static float   kc[3];

static void draw_quad(const gq *q)
{
    const gx_tex *t = q->tex;
    if (!t) return;

    /* the scene clip window (l r t b, inclusive) */
    const int cx0 = q->clip[0] < 0 ? 0 : q->clip[0], cx1 = q->clip[1] > SW - 1 ? SW - 1 : q->clip[1];
    const int cy0 = q->clip[2] < 0 ? 0 : q->clip[2], cy1 = q->clip[3] > SH - 1 ? SH - 1 : q->clip[3];
    if (cx0 > cx1 || cy0 > cy1) return;

    /* texcoords against the bake: texel (u - min_u) / step at its centre (texel_centre, as ss22_gl.c draws) */
    const float iu = 1.0f / ((float)t->step_u * (float)t->tw), iv = 1.0f / ((float)t->step_v * (float)t->th);
    sv_t sv[10], cv[32];
    for (int i = 0; i < q->nrv; i++) {
        const gq_vert *v = &q->rv[i];
        const float tu = ((float)((int)v->u - (int)t->min_u) + 0.5f) * iu;
        const float tv = ((float)((int)v->v - (int)t->min_v) + 0.5f) * iv;
        const float iw = v->z > 0 ? 1.0f / (float)v->z : 1.0f;
        sv[i].x = (float)v->sx16 / 16.0f; sv[i].y = (float)v->sy16 / 16.0f;
        sv[i].s = tu * iw; sv[i].t = tv * iw; sv[i].w = iw;
        sv[i].bri = v->bri > 0 ? (float)v->bri / 64.0f : 0.0f;
    }
    const int n = clip_to_screen(sv, q->nrv, cv);
    if (n < 3) return;

    const uint8_t *ftab = NULL;
    int fsd = 0;
    const int fogged = g_fog_valid && !(q->cz_adjust & 0x800000) && fog_quad(q->color, q->cz_type, &ftab, &fsd);

    gx_mode_quads();
    if (cx0 != sc[0] || cy0 != sc[1] || cx1 - cx0 + 1 != sc[2] || cy1 - cy0 + 1 != sc[3]) {
        sc[0] = cx0; sc[1] = cy0; sc[2] = cx1 - cx0 + 1; sc[3] = cy1 - cy0 + 1;
        GX_SetScissor((u32)sc[0], (u32)sc[1], (u32)sc[2], (u32)sc[3]);
    }
    const int slot = tlut_slot((int)((q->color >> 8) & 0x7F));
    if (t != bound || slot != bound_slot) {
        GXTexObj to;
        GX_InitTexObjCI(&to, t->data, t->tw, t->th, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0 + slot);
        GX_InitTexObjFilterMode(&to, GX_NEAR, GX_NEAR);
        GX_LoadTexObj(&to, GX_TEXMAP0);
        bound = (gx_tex *)t; bound_slot = slot;
    }

    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, (u16)n);
    for (int i = 0; i < n; i++) {
        const float w = cv[i].w > 0.0f ? cv[i].w : 1.0f, Z = 1.0f / w;
        int r = (int)(cv[i].bri * kc[0] + 0.5f), g = (int)(cv[i].bri * kc[1] + 0.5f), b = (int)(cv[i].bri * kc[2] + 0.5f);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        const int a = fogged ? fog_alpha_for_z(ftab, fsd, (int32_t)Z) : 255;
        GX_Position3f32(cv[i].x * Z, cv[i].y * Z, -Z);
        GX_Color4u8((u8)r, (u8)g, (u8)b, (u8)a);
        GX_TexCoord2f32(cv[i].s / w, cv[i].t / w);
    }
    GX_End();
    drawn++;
}

/* one sprite, in its turn (the scissor back to the full screen: a sprite has no clip window) */
static void draw_sprite(int i)
{
    if (!itex[i].tex) return;
    if (sc[0] != 0 || sc[1] != 0 || sc[2] != SW || sc[3] != SH) {
        sc[0] = 0; sc[1] = 0; sc[2] = SW; sc[3] = SH;
        GX_SetScissor(0, 0, SW, SH);
    }
    gx_picture(itex[i].tex, itex[i].tw, itex[i].th, items[i].x0, items[i].y0, items[i].w, items[i].h);
}

void ss22_draw(int vw, int vh)
{
    (void)vw; (void)vh;
    if (!ready || !wii_port_gx_ready()) return;
    const uint32_t frame = frame_no + 1;               /* never 0: a fresh entry's frame 0 is "not drawn yet" */

    /* 1. every texture and palette this frame needs, before the GPU is given anything (an eviction never touches a texture
     *    a queued quad still reads: the previous frame's GPU work finished in wii_port_gx_present) */
    uint64_t t0 = wii_port_usec();
    for (int i = 0; i < qn; i++) {
        gq *q = &qbuf[i];
        int min_u = q->uvbox[0], max_u = q->uvbox[1], min_v = q->uvbox[2], max_v = q->uvbox[3];
        int range_u = max_u - min_u + 1, range_v = max_v - min_v + 1;
        if (range_u < 1) range_u = 1;
        if (range_v < 1) range_v = 1;
        if (range_u < 16) { min_u = (min_u * 2 + range_u) / 2 - 8; range_u = 16; }   /* tex_bake.c's widening */
        if (range_v < 16) { min_v = (min_v * 2 + range_v) / 2 - 8; range_v = 16; }
        if (min_u < 0) min_u = 0;
        if (min_v < 0) min_v = 0;
        q->tex = tex_for(min_u, min_v, range_u, range_v, q->texbank, q->cmode & 0xF, frame);
        tlut_refresh((int)((q->color >> 8) & 0x7F));
    }
    uint64_t t1 = wii_port_usec();
    st_tex += t1 - t0;

    /* 2. the 2D: each sprite into its own small texture, the text layer only when it changed */
    if (have_spr) sprites_render();
    t0 = wii_port_usec();
    st_spr += t0 - t1;
    if (have_text) text_update();
    t1 = wii_port_usec();
    st_txt += t1 - t0;
    t0 = t1;

    /* 3. the frame */
    gx_common_state();
    gx_mode = MODE_NONE;
    gx_mode_flat(false);
    gx_rect(g_fog_valid ? g_fog.bg[0] : 0, g_fog_valid ? g_fog.bg[1] : 0, g_fog_valid ? g_fog.bg[2] : 0, 255);

    int fade[3] = { 256, 256, 256 };                  /* the poly fade, /256 (ss22_gl.c's ss22_fade_rgb) */
    if (g_fog_valid && g_fog.poly_fade_enabled)
        for (int c = 0; c < 3; c++) fade[c] = g_fog.poly_fade[c];
    quads_fog = (GXColor){ 0, 0, 0, 255 };
    if (g_fog_valid) {
        quads_fog.r = (uint8_t)(g_fog.fog_rgb[0] * fade[0] / 256);
        quads_fog.g = (uint8_t)(g_fog.fog_rgb[1] * fade[1] / 256);
        quads_fog.b = (uint8_t)(g_fog.fog_rgb[2] * fade[2] / 256);
    }
    /* the vertex colour is shade/4 = bri/256 (TEV scales by 4), times the fade */
    for (int c = 0; c < 3; c++) kc[c] = 255.0f / 4.0f * (float)fade[c] / 256.0f;

    /* the MERGED z order (ss22_gl.c): quads and sprites far to near, a sprite before a quad at the same z */
    bound = NULL; bound_slot = -1; drawn = 0;
    sc[0] = 0; sc[1] = 0; sc[2] = SW; sc[3] = SH;
    int qi = 0, si = 0;
    while (qi < qn || si < ni) {
        const uint32_t qz = qi < qn ? (uint32_t)(qbuf[qkeys[qi].idx].zsort & 0xFFFFFF) : 0;
        const uint32_t sz = si < ni ? items[si].z : 0;
        if (si < ni && (qi >= qn || sz >= qz)) draw_sprite(si++);
        else draw_quad(&qbuf[qkeys[qi++].idx]);
    }
    if (sc[0] != 0 || sc[1] != 0 || sc[2] != SW || sc[3] != SH) GX_SetScissor(0, 0, SW, SH);
    st_drawn += drawn;

    if (g_fog_valid && (g_fog.mixer_flags & 1) && g_fog.screen_fade_factor) {
        /* blend(rgb, fade, 0xff - factor): the fade colour at alpha (factor + 1) / 256 */
        gx_mode_flat(true);
        gx_rect(g_fog.screen_fade[0], g_fog.screen_fade[1], g_fog.screen_fade[2],
                (uint8_t)(((g_fog.screen_fade_factor + 1) * 255 + 128) / 256));
    }
    if (have_text && txt_visible) {
        gx_text_layer();
        for (int i = 0; i < ni; i++)                  /* the sprites over the text layer, over it again */
            if (items[i].prioverchar) draw_sprite(i);
    }
    gx_crosshair();
    GX_Flush();
    st_gx += wii_port_usec() - t0;
    stats_frame();
}

int ss22_quads(void)   { return quads_last; }
int ss22_sprites(void) { return sprites_last; }

/* the software path's interface (the frame is in the EFB now, wii_port_gx_present shows it) */
const uint16_t *ss22_wii_frame_rgb565(int *w, int *h) { if (w) *w = SW; if (h) *h = SH; return NULL; }
bool ss22_wii_frame_has_content(void) { return quads_last > 0 || ni > 0 || have_text; }
bool ss22_wii_frame_is_visible(void)  { return ready; }

void eng_gl_context_attributes(void) {}
bool eng_gl_open_headless(int w, int h) { (void)w; (void)h; return false; }
bool eng_gl_write_ppm(const char *path, int vw, int vh) { (void)path; (void)vw; (void)vh; return false; }

#endif
