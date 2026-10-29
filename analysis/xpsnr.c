/*
 * Copyright (c) 2024 Christian R. Helmrich
 * Copyright (c) 2024 Christian Lehmann
 * Copyright (c) 2024 Christian Stoffers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Calculate the extended perceptually weighted PSNR (XPSNR) between two input videos.
 *
 * Authors: Christian Helmrich, Lehmann, and Stoffers, Fraunhofer HHI, Berlin, Germany
 */

/* Standalone pixel adapter: math below is derived from vf_xpsnr.c at
 * Nakiha/FFmpeg 8998a1a670c81a04d144c36f9e2d3aa680c10720,
 * SHA-256 4b59b32eb4a501ac74f6d767a916ef1a29833e1895b150cfa96b8e4ffb91f3d7.
 * Retains the pinned scalar algorithm, including clip-reset history and pooling.
 * No AVFilter, framesync, timestamps, conversions, file IO or host processes.
 * Changes: minimal state, caller-owned packed planes, scalar SSE loop, explicit
 * lifecycle/budgets, per-frame values and final pool returned directly as doubles.
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define XPSNR_GAMMA 2

typedef struct {
    uint64_t (*highds_func)(int,int,int,int,const int16_t *,int);
    uint64_t (*diff1st_func)(uint32_t,uint32_t,const int16_t *,int16_t *,int);
    uint64_t (*diff2nd_func)(uint32_t,uint32_t,const int16_t *,int16_t *,int16_t *,int);
} XPSNRDSPContext;
typedef struct {
    int bpp, depth, num_comps, line_sizes[3], plane_height[3], plane_width[3];
    unsigned frame_rate;
    uint64_t num_frames_64, max_error_64;
    double *sse_luma, *weights, sum_wdist[3], sum_xpsnr[3], current[3];
    int16_t *buf_org_m1, *buf_org_m2, *buf_org[3], *buf_rec[3];
    unsigned input_bytes, max_frames;
    int opened, flushed, failed;
    XPSNRDSPContext dsp;
} XPSNRContext;
static XPSNRContext xpsnr;

static uint64_t highds(const int x_act, const int y_act, const int w_act, const int h_act, const int16_t *o_m0, const int o)
{
    uint64_t sa_act = 0;

    for (int y = y_act; y < h_act; y += 2) {
        for (int x = x_act; x < w_act; x += 2) {
            const int f = 12 * ((int)o_m0[ y   *o + x  ] + (int)o_m0[ y   *o + x+1] + (int)o_m0[(y+1)*o + x  ] + (int)o_m0[(y+1)*o + x+1])
                         - 3 * ((int)o_m0[(y-1)*o + x  ] + (int)o_m0[(y-1)*o + x+1] + (int)o_m0[(y+2)*o + x  ] + (int)o_m0[(y+2)*o + x+1])
                         - 3 * ((int)o_m0[ y   *o + x-1] + (int)o_m0[ y   *o + x+2] + (int)o_m0[(y+1)*o + x-1] + (int)o_m0[(y+1)*o + x+2])
                         - 2 * ((int)o_m0[(y-1)*o + x-1] + (int)o_m0[(y-1)*o + x+2] + (int)o_m0[(y+2)*o + x-1] + (int)o_m0[(y+2)*o + x+2])
                             - ((int)o_m0[(y-2)*o + x-1] + (int)o_m0[(y-2)*o + x  ] + (int)o_m0[(y-2)*o + x+1] + (int)o_m0[(y-2)*o + x+2]
                              + (int)o_m0[(y+3)*o + x-1] + (int)o_m0[(y+3)*o + x  ] + (int)o_m0[(y+3)*o + x+1] + (int)o_m0[(y+3)*o + x+2]
                              + (int)o_m0[(y-1)*o + x-2] + (int)o_m0[ y   *o + x-2] + (int)o_m0[(y+1)*o + x-2] + (int)o_m0[(y+2)*o + x-2]
                              + (int)o_m0[(y-1)*o + x+3] + (int)o_m0[ y   *o + x+3] + (int)o_m0[(y+1)*o + x+3] + (int)o_m0[(y+2)*o + x+3]);
            sa_act += (uint64_t) abs(f);
        }
    }
    return sa_act;
}

static uint64_t diff1st(const uint32_t w_act, const uint32_t h_act, const int16_t *o_m0, int16_t *o_m1, const int o)
{
    uint64_t ta_act = 0;

    for (uint32_t y = 0; y < h_act; y += 2) {
        for (uint32_t x = 0; x < w_act; x += 2) {
            const int t = (int)o_m0[y*o + x] + (int)o_m0[y*o + x+1] + (int)o_m0[(y+1)*o + x] + (int)o_m0[(y+1)*o + x+1]
                       - ((int)o_m1[y*o + x] + (int)o_m1[y*o + x+1] + (int)o_m1[(y+1)*o + x] + (int)o_m1[(y+1)*o + x+1]);
            ta_act += (uint64_t) abs(t);
            o_m1[y*o + x  ] = o_m0[y*o + x  ];  o_m1[(y+1)*o + x  ] = o_m0[(y+1)*o + x  ];
            o_m1[y*o + x+1] = o_m0[y*o + x+1];  o_m1[(y+1)*o + x+1] = o_m0[(y+1)*o + x+1];
        }
    }
    return (ta_act * XPSNR_GAMMA);
}

static uint64_t diff2nd(const uint32_t w_act, const uint32_t h_act, const int16_t *o_m0, int16_t *o_m1, int16_t *o_m2, const int o)
{
    uint64_t ta_act = 0;

    for (uint32_t y = 0; y < h_act; y += 2) {
        for (uint32_t x = 0; x < w_act; x += 2) {
            const int t = (int)o_m0[y*o + x] + (int)o_m0[y*o + x+1] + (int)o_m0[(y+1)*o + x] + (int)o_m0[(y+1)*o + x+1]
                   - 2 * ((int)o_m1[y*o + x] + (int)o_m1[y*o + x+1] + (int)o_m1[(y+1)*o + x] + (int)o_m1[(y+1)*o + x+1])
                        + (int)o_m2[y*o + x] + (int)o_m2[y*o + x+1] + (int)o_m2[(y+1)*o + x] + (int)o_m2[(y+1)*o + x+1];
            ta_act += (uint64_t) abs(t);
            o_m2[y*o + x  ] = o_m1[y*o + x  ];  o_m2[(y+1)*o + x  ] = o_m1[(y+1)*o + x  ];
            o_m2[y*o + x+1] = o_m1[y*o + x+1];  o_m2[(y+1)*o + x+1] = o_m1[(y+1)*o + x+1];
            o_m1[y*o + x  ] = o_m0[y*o + x  ];  o_m1[(y+1)*o + x  ] = o_m0[(y+1)*o + x  ];
            o_m1[y*o + x+1] = o_m0[y*o + x+1];  o_m1[(y+1)*o + x+1] = o_m0[(y+1)*o + x+1];
        }
    }
    return (ta_act * XPSNR_GAMMA);
}

static inline uint64_t calc_squared_error(XPSNRContext const *s,
                                          const int16_t *blk_org,     const uint32_t stride_org,
                                          const int16_t *blk_rec,     const uint32_t stride_rec,
                                          const uint32_t block_width, const uint32_t block_height)
{
    uint64_t sse = 0;  /* sum of squared errors */

    for (uint32_t y = 0; y < block_height; y++) {
        for (uint32_t x = 0; x < block_width; x++) {
            const int delta = (int)blk_org[x] - (int)blk_rec[x];
            sse += (uint64_t)(delta * delta);
        }
        blk_org += stride_org;
        blk_rec += stride_rec;
    }

    /* return nonweighted sum of squared errors */
    return sse;
}

static inline double calc_squared_error_and_weight (XPSNRContext const *s,
                                                    const int16_t *pic_org,     const uint32_t stride_org,
                                                    int16_t       *pic_org_m1,  int16_t       *pic_org_m2,
                                                    const int16_t *pic_rec,     const uint32_t stride_rec,
                                                    const uint32_t offset_x,    const uint32_t offset_y,
                                                    const uint32_t block_width, const uint32_t block_height,
                                                    const uint32_t bit_depth,   const uint32_t int_frame_rate, double *ms_act)
{
    const int         o = (int) stride_org;
    const int         r = (int) stride_rec;
    const int16_t *o_m0 = pic_org    + offset_y * o + offset_x;
    int16_t       *o_m1 = pic_org_m1 + offset_y * o + offset_x;
    int16_t       *o_m2 = pic_org_m2 + offset_y * o + offset_x;
    const int16_t *r_m0 = pic_rec    + offset_y * r + offset_x;
    const int     b_val = (s->plane_width[0] * s->plane_height[0] > 2048 * 1152 ? 2 : 1); /* threshold is a bit more than HD resolution */
    const int     x_act = (offset_x > 0 ? 0 : b_val);
    const int     y_act = (offset_y > 0 ? 0 : b_val);
    const int     w_act = (offset_x + block_width  < (uint32_t) s->plane_width [0] ? (int) block_width  : (int) block_width  - b_val);
    const int     h_act = (offset_y + block_height < (uint32_t) s->plane_height[0] ? (int) block_height : (int) block_height - b_val);

    const double sse = (double) calc_squared_error (s, o_m0, stride_org,
                                                    r_m0, stride_rec,
                                                    block_width, block_height);
    uint64_t sa_act = 0;  /* spatial abs. activity */
    uint64_t ta_act = 0; /* temporal abs. activity */

    if (w_act <= x_act || h_act <= y_act) /* small */
        return sse;

    if (b_val > 1) { /* highpass with downsampling */
        if (w_act > 12)
            sa_act = s->dsp.highds_func(x_act, y_act, w_act, h_act, o_m0, o);
        else
            highds(x_act, y_act, w_act, h_act, o_m0, o);
    } else { /* <=HD highpass without downsampling */
        for (int y = y_act; y < h_act; y++) {
            for (int x = x_act; x < w_act; x++) {
                const int f = 12 * (int)o_m0[y*o + x] - 2 * ((int)o_m0[y*o + x-1] + (int)o_m0[y*o + x+1] + (int)o_m0[(y-1)*o + x] + (int)o_m0[(y+1)*o + x])
                                 - ((int)o_m0[(y-1)*o + x-1] + (int)o_m0[(y-1)*o + x+1] + (int)o_m0[(y+1)*o + x-1] + (int)o_m0[(y+1)*o + x+1]);
                sa_act += (uint64_t) abs(f);
            }
        }
    }

    /* calculate weight (average squared activity) */
    *ms_act = (double) sa_act / ((double) (w_act - x_act) * (double) (h_act - y_act));

    if (b_val > 1) { /* highpass with downsampling */
        if (int_frame_rate < 32) /* 1st-order diff */
            ta_act = s->dsp.diff1st_func(block_width, block_height, o_m0, o_m1, o);
        else /* 2nd-order diff (diff of two diffs) */
            ta_act = s->dsp.diff2nd_func(block_width, block_height, o_m0, o_m1, o_m2, o);
    } else { /* <=HD highpass without downsampling */
        if (int_frame_rate < 32) { /* 1st-order diff */
            for (uint32_t y = 0; y < block_height; y++) {
                for (uint32_t x = 0; x < block_width; x++) {
                    const int t = (int)o_m0[y * o + x] - (int)o_m1[y * o + x];

                    ta_act += XPSNR_GAMMA * (uint64_t) abs(t);
                    o_m1[y * o + x] = o_m0[y * o + x];
                }
            }
        } else { /* 2nd-order diff (diff of 2 diffs) */
            for (uint32_t y = 0; y < block_height; y++) {
                for (uint32_t x = 0; x < block_width; x++) {
                    const int t = (int)o_m0[y * o + x] - 2 * (int)o_m1[y * o + x] + (int)o_m2[y * o + x];

                    ta_act += XPSNR_GAMMA * (uint64_t) abs(t);
                    o_m2[y * o + x] = o_m1[y * o + x];
                    o_m1[y * o + x] = o_m0[y * o + x];
                }
            }
        }
    }

    /* weight += mean squared temporal activity */
    *ms_act += (double) ta_act / ((double) block_width * (double) block_height);

    /* lower limit, accounts for high-pass gain */
    if (*ms_act < (double) (1 << (bit_depth - 6)))
        *ms_act = (double) (1 << (bit_depth - 6));

    *ms_act *= *ms_act; /* since SSE is squared */

    /* return nonweighted sum of squared errors */
    return sse;
}

static inline double get_avg_xpsnr (const double sqrt_wsse_val,  const double sum_xpsnr_val,
                                    const uint32_t image_width,  const uint32_t image_height,
                                    const uint64_t max_error_64, const uint64_t num_frames_64)
{
    if (num_frames_64 == 0)
        return INFINITY;

    if (sqrt_wsse_val >= (double) num_frames_64) { /* square-mean-root average */
        const double avg_dist = sqrt_wsse_val / (double) num_frames_64;
        const uint64_t  num64 = (uint64_t) image_width * (uint64_t) image_height * max_error_64;

        return 10.0 * log10((double) num64 / ((double) avg_dist * (double) avg_dist));
    }

    return sum_xpsnr_val / (double) num_frames_64; /* older log-domain average */
}

static int get_wsse(XPSNRContext *s, int16_t **org, int16_t *org_m1,
                    int16_t *org_m2, int16_t **rec, uint64_t *const wsse64)
{
    const uint32_t       w = s->plane_width [0]; /* luma image width in pixels */
    const uint32_t       h = s->plane_height[0];/* luma image height in pixels */
    const double         r = (double)(w * h) / (3840.0 * 2160.0); /* UHD ratio */
    const uint32_t       b = FFMAX(1, 4 * (int32_t) (32.0 * sqrt(r) +
                                                     0.5)); /* block size, integer multiple of 4 for SIMD */
    const uint32_t   w_blk = (w + b - 1) / b; /* luma width in units of blocks */
    const double   avg_act = sqrt(16.0 * (double) (1 << (2 * s->depth - 9)) / sqrt(FFMAX(0.00001,
                                                                                   r))); /* the sqrt(a_pic) */
    const int  *stride_org = (s->bpp == 1 ? s->plane_width : s->line_sizes);
    uint32_t x, y, idx_blk = 0; /* the "16.0" above is due to fixed-point code */
    double *const sse_luma = s->sse_luma;
    double *const  weights = s->weights;
    int c;

    if (!wsse64 || (s->depth < 6) || (s->depth > 16) || (s->num_comps <= 0) ||
        (s->num_comps > 3) || (w == 0) || (h == 0)) {
        return (-EINVAL);
    }
    if (!weights || (b >= 4 && !sse_luma)) {
        return (-ENOMEM);
    }

    if (b >= 4) {
        const int16_t *p_org = org[0];
        const uint32_t s_org = stride_org[0] / s->bpp;
        const int16_t *p_rec = rec[0];
        const uint32_t s_rec = s->plane_width[0];
        double     wsse_luma = 0.0;

        for (y = 0; y < h; y += b) { /* calculate block SSE and perceptual weights */
            const uint32_t block_height = (y + b > h ? h - y : b);

            for (x = 0; x < w; x += b, idx_blk++) {
                const uint32_t block_width = (x + b > w ? w - x : b);
                double ms_act = 1.0, ms_act_prev = 0.0;

                sse_luma[idx_blk] = calc_squared_error_and_weight(s, p_org, s_org,
                                                                  org_m1 /* pixel  */,
                                                                  org_m2 /* memory */,
                                                                  p_rec, s_rec,
                                                                  x, y,
                                                                  block_width, block_height,
                                                                  s->depth, s->frame_rate, &ms_act);
                weights[idx_blk] = 1.0 / sqrt(ms_act);

                if (w * h <= 640 * 480) { /* in-line "min-smoothing" as in paper */
                    if (x == 0) /* first column */
                        ms_act_prev = (idx_blk > 1 ? weights[idx_blk - 2] : 0);
                    else  /* after first column */
                        ms_act_prev = (x > b ? FFMAX(weights[idx_blk - 2], weights[idx_blk]) : weights[idx_blk]);

                    if (idx_blk > w_blk) /* after the first row and first column */
                        ms_act_prev = FFMAX(ms_act_prev, weights[idx_blk - 1 - w_blk]); /* min (L, T) */
                    if ((idx_blk > 0) && (weights[idx_blk - 1] > ms_act_prev))
                        weights[idx_blk - 1] = ms_act_prev;

                    if ((x + b >= w) && (y + b >= h) && (idx_blk > w_blk)) { /* last block in picture */
                        ms_act_prev = FFMAX(weights[idx_blk - 1], weights[idx_blk - w_blk]);
                        if (weights[idx_blk] > ms_act_prev)
                            weights[idx_blk] = ms_act_prev;
                    }
                }
            } /* for x */
        } /* for y */

        for (y = idx_blk = 0; y < h; y += b) { /* calculate sum for luma (Y) XPSNR */
            for (x = 0; x < w; x += b, idx_blk++) {
                wsse_luma += sse_luma[idx_blk] * weights[idx_blk];
            }
        }
        wsse64[0] = (wsse_luma <= 0.0 ? 0 : (uint64_t) (wsse_luma * avg_act + 0.5));
    } /* b >= 4 */

    for (c = 0; c < s->num_comps; c++) { /* finalize WSSE value for each component */
        const int16_t *p_org = org[c];
        const uint32_t s_org = stride_org[c] / s->bpp;
        const int16_t *p_rec = rec[c];
        const uint32_t s_rec = s->plane_width[c];
        const uint32_t w_pln = s->plane_width[c];
        const uint32_t h_pln = s->plane_height[c];

        if (b < 4) /* picture is too small for XPSNR, calculate nonweighted PSNR */
            wsse64[c] = calc_squared_error (s, p_org, s_org,
                                            p_rec, s_rec,
                                            w_pln, h_pln);
        else if (c > 0) { /* b >= 4 so Y XPSNR has already been calculated above */
            const uint32_t  bx = (b * w_pln) / w;
            const uint32_t  by = (b * h_pln) / h;  /* up to chroma downsampling by 4 */
            double wsse_chroma = 0.0;

            for (y = idx_blk = 0; y < h_pln; y += by) { /* calc chroma (Cb/Cr) XPSNR */
                const uint32_t block_height = (y + by > h_pln ? h_pln - y : by);

                for (x = 0; x < w_pln; x += bx, idx_blk++) {
                    const uint32_t block_width = (x + bx > w_pln ? w_pln - x : bx);

                    wsse_chroma += (double) calc_squared_error (s, p_org + y * s_org + x, s_org,
                                                                p_rec + y * s_rec + x, s_rec,
                                                                block_width, block_height) * weights[idx_blk];
                }
            }
            wsse64[c] = (wsse_chroma <= 0.0 ? 0 : (uint64_t) (wsse_chroma * avg_act + 0.5));
        }
    } /* for c */

    return 0;
}


int vpa_xpsnr_abi(void) { return 1; }
int vpa_xpsnr_close(void) {
    free(xpsnr.sse_luma); free(xpsnr.weights);
    free(xpsnr.buf_org_m1); free(xpsnr.buf_org_m2);
    for (unsigned c=0;c<3;c++) { free(xpsnr.buf_org[c]); free(xpsnr.buf_rec[c]); }
    memset(&xpsnr,0,sizeof(xpsnr));
    return 0;
}
int vpa_xpsnr_open(unsigned width,unsigned height,unsigned depth,
                   unsigned rate_num,unsigned rate_den,unsigned max_frames) {
    XPSNRContext *s=&xpsnr;
    if(s->opened) return -EBUSY;
    if(width<64 || height<64 || width>4096 || height>4096 || width*height>4096*2304 ||
       (width&1) || (height&1) || (depth!=8 && depth!=10) || !rate_num || !rate_den ||
       !max_frames || max_frames>1000000) return -EINVAL;
    memset(s,0,sizeof(*s));
    s->opened=1; s->bpp=depth==8?1:2; s->depth=depth; s->num_comps=3;
    s->frame_rate=rate_num/rate_den; s->max_frames=max_frames;
    s->input_bytes=width*height*3/2*s->bpp;
    s->max_error_64=((1u<<depth)-1)*((1u<<depth)-1);
    const unsigned b=FFMAX(1,4*(int32_t)(32.0*sqrt((double)(width*height)/(3840.0*2160.0))+0.5));
    const unsigned blocks=((width+b-1)/b)*((height+b-1)/b);
    s->sse_luma=calloc(blocks,sizeof(double)); s->weights=calloc(blocks,sizeof(double));
    s->buf_org_m1=calloc(width*height,sizeof(int16_t));
    s->buf_org_m2=calloc(width*height,sizeof(int16_t));
    if(!s->sse_luma || !s->weights || !s->buf_org_m1 || !s->buf_org_m2) goto oom;
    for(unsigned c=0;c<3;c++) {
        const unsigned w=c?width/2:width,h=c?height/2:height;
        s->plane_width[c]=w; s->plane_height[c]=h; s->line_sizes[c]=w*s->bpp;
        s->buf_org[c]=malloc(w*h*sizeof(int16_t)); s->buf_rec[c]=malloc(w*h*sizeof(int16_t));
        if(!s->buf_org[c] || !s->buf_rec[c]) goto oom;
    }
    s->dsp=(XPSNRDSPContext){highds,diff1st,diff2nd};
    return 0;
oom:
    vpa_xpsnr_close(); return -ENOMEM;
}
int vpa_xpsnr_feed(const uint8_t *reference,const uint8_t *distorted,unsigned bytes,unsigned index) {
    XPSNRContext *s=&xpsnr;
    if(!s->opened || s->flushed || s->failed || !reference || !distorted ||
       bytes!=s->input_bytes || index!=s->num_frames_64 || index>=s->max_frames) return -EINVAL;
    unsigned offset=0;
    for(unsigned c=0;c<3;c++) {
        const unsigned samples=s->plane_width[c]*s->plane_height[c];
        for(unsigned n=0;n<samples;n++) {
            const unsigned at=offset+n*s->bpp;
            const unsigned r=s->bpp==1?reference[at]:reference[at]|(reference[at+1]<<8);
            const unsigned d=s->bpp==1?distorted[at]:distorted[at]|(distorted[at+1]<<8);
            if(r>=(1u<<s->depth) || d>=(1u<<s->depth)) {s->failed=1;return -EINVAL;}
            s->buf_org[c][n]=r; s->buf_rec[c][n]=d;
        }
        offset+=samples*s->bpp;
    }
    uint64_t wsse[3]={0};
    const int err=get_wsse(s,s->buf_org,s->buf_org_m1,s->buf_org_m2,s->buf_rec,wsse);
    if(err) {s->failed=1;return err;}
    for(unsigned c=0;c<3;c++) {
        const double root=sqrt((double)wsse[c]);
        s->current[c]=get_avg_xpsnr(root,INFINITY,s->plane_width[c],s->plane_height[c],s->max_error_64,1);
        s->sum_wdist[c]+=root; s->sum_xpsnr[c]+=s->current[c];
    }
    s->num_frames_64++;
    return 0;
}
int vpa_xpsnr_flush(void) {
    if(!xpsnr.opened || xpsnr.failed || xpsnr.flushed || xpsnr.num_frames_64!=xpsnr.max_frames) return -EINVAL;
    xpsnr.flushed=1; return 0;
}
double vpa_xpsnr_score(unsigned plane) {
    return !xpsnr.opened || xpsnr.failed || !xpsnr.num_frames_64 || plane>=3 ? NAN : xpsnr.current[plane];
}
double vpa_xpsnr_pool(unsigned plane) {
    XPSNRContext *s=&xpsnr;
    if(!s->opened || !s->flushed || s->failed || plane>=3) return NAN;
    return get_avg_xpsnr(s->sum_wdist[plane],s->sum_xpsnr[plane],s->plane_width[plane],s->plane_height[plane],s->max_error_64,s->num_frames_64);
}
