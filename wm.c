/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>

#include "ksim.h"
#include "write-png.h"

struct edge {
	int32_t a, b, c, bias;
	int32_t min_x, min_y;
};

struct payload {
	int x0, y0;
	int32_t start_w2, start_w0, start_w1;
	int32_t area;
	float inv_area;
	struct reg w2, w0, w1;
	struct edge e01, e12, e20;

	struct {
		struct reg offsets;
		void *buffer;
	} depth;

	int min_x, min_y, max_x, max_y;
	int32_t row_w2, row_w0, row_w1;

	float w_deltas[4];
	struct reg attribute_deltas[64];
	struct thread t;
};

/* Decode this at jit time and put in constant pool. */

bool
get_surface(uint32_t binding_table_offset, int i, struct surface *s)
{
	uint64_t offset, range;
	const uint32_t *binding_table;
	const uint32_t *state;

	binding_table = map_gtt_offset(binding_table_offset +
				       gt.surface_state_base_address, &range);
	if (range < 4)
		return false;

	state = map_gtt_offset(binding_table[i] +
			       gt.surface_state_base_address, &range);
	if (range < 16 * 4)
		return false;

	struct GEN9_RENDER_SURFACE_STATE v;
	GEN9_RENDER_SURFACE_STATE_unpack(state, &v);

	s->width = v.Width + 1;
	s->height = v.Height + 1;
	s->stride = v.SurfacePitch + 1;
	s->format = v.SurfaceFormat;
	s->cpp = format_size(s->format);

	offset = v.SurfaceBaseAddress;
	s->pixels = map_gtt_offset(offset, &range);
	if (range < s->height * s->stride) {
		ksim_warn("surface state out-of-range for bo\n");
		return false;
	}

	return true;
}

void
sfid_render_cache_rt_write_simd8(struct thread *t,
				 const struct sfid_render_cache_args *args)
{
	int x = t->grf[1].ud[2] & 0xffff;
	int y = t->grf[1].ud[2] >> 16;
	__m256i r, g, b, a, shift;
	struct reg argb;
	__m256 scale;
	scale = _mm256_set1_ps(255.0f);
	struct reg *src = &t->grf[args->src];

	if (x >= args->rt.width || y >= args->rt.height)
		return;

	r = _mm256_cvtps_epi32(_mm256_mul_ps(src[0].reg, scale));
	g = _mm256_cvtps_epi32(_mm256_mul_ps(src[1].reg, scale));
	b = _mm256_cvtps_epi32(_mm256_mul_ps(src[2].reg, scale));
	a = _mm256_cvtps_epi32(_mm256_mul_ps(src[3].reg, scale));

	shift = _mm256_set1_epi32(8);
	argb.ireg = _mm256_sllv_epi32(a, shift);
	argb.ireg = _mm256_or_si256(argb.ireg, r);
	argb.ireg = _mm256_sllv_epi32(argb.ireg, shift);
	argb.ireg = _mm256_or_si256(argb.ireg, g);
	argb.ireg = _mm256_sllv_epi32(argb.ireg, shift);
	argb.ireg = _mm256_or_si256(argb.ireg, b);

#define SWIZZLE(x, y, z, w) \
	( ((x) << 0) | ((y) << 2) | ((z) << 4) | ((w) << 6) )

	/* Swizzle two middle pixel pairs so that dword 0-3 and 4-7
	 * form linear owords of pixels. */
	argb.ireg = _mm256_permute4x64_epi64(argb.ireg, SWIZZLE(0, 2, 1, 3));
	__m256i mask = _mm256_permute4x64_epi64(t->mask_full, SWIZZLE(0, 2, 1, 3));

	const int tile_x = x * args->rt.cpp / 512;
	const int tile_y = y / 8;
	const int tile_stride = args->rt.stride / 512;
	void *tile_base =
		args->rt.pixels + (tile_x + tile_y * tile_stride) * 4096;

	const int ix = x & (512 / args->rt.cpp - 1);
	const int iy = y & 7;
	void *base = tile_base + ix * args->rt.cpp + iy * 512;

	_mm_maskstore_epi32(base,
			    _mm256_extractf128_si256(mask, 0),
			    _mm256_extractf128_si256(argb.ireg, 0));
	_mm_maskstore_epi32(base + 512,
			    _mm256_extractf128_si256(mask, 1),
			    _mm256_extractf128_si256(argb.ireg, 1));
}

static uint32_t
depth_test(struct payload *p, uint32_t mask, int x, int y)
{
	uint32_t cpp = depth_format_size(gt.depth.format);

	if (x >= gt.depth.width || y >= gt.depth.height + 1)
		return 0;

	/* early depth test */
	struct reg w, w_unorm;
	w.reg = _mm256_fmadd_ps(_mm256_set1_ps(p->w_deltas[0]), p->w1.reg,
				_mm256_fmadd_ps(_mm256_set1_ps(p->w_deltas[1]), p->w2.reg,
						_mm256_set1_ps(p->w_deltas[3])));

	struct reg d24x8, cmp, d_f;

	/* Y-tiled depth buffer */
	const int tile_x = x * cpp / 128;
	const int tile_y = y / 32;
	const int tile_stride = gt.depth.stride / 128;
	void *tile_base =
		p->depth.buffer + (tile_x + tile_y * tile_stride) * 4096;

	const int ix = x & (128 / cpp - 1);
	const int iy = y & 31;
	void *base = tile_base + ix * cpp * 32 + iy * 16;

	switch (gt.depth.format) {
	case D32_FLOAT:
		d_f.reg = _mm256_load_ps(base);
		break;
	case D24_UNORM_X8_UINT:
		d24x8.ireg = _mm256_load_si256(base);
		d_f.reg = _mm256_mul_ps(_mm256_cvtepi32_ps(d24x8.ireg),
					_mm256_set1_ps(1.0f / 16777216.0f));
		break;
	case D16_UNORM:
		stub("D16_UNORM");
	default:
		ksim_unreachable("invalid depth format");
	}

	/* Swizzle two middle pixel pairs so that dword 0-3 and 4-7
	 * form linear owords of pixels. */
	d_f.ireg = _mm256_permute4x64_epi64(d_f.ireg, SWIZZLE(0, 2, 1, 3));

	if (gt.depth.test_enable) {
		cmp.reg = _mm256_cmp_ps(d_f.reg, w.reg, _CMP_LT_OS);
		cmp.ireg = _mm256_and_si256(cmp.ireg, p->t.mask_full);
		p->t.mask_full = cmp.ireg;
		mask = _mm256_movemask_ps(cmp.reg);
	}

	if (gt.depth.write_enable) {
		w_unorm.ireg = _mm256_cvtps_epi32(_mm256_mul_ps(w.reg, _mm256_set1_ps((1 << 24) - 1)));
		w_unorm.ireg = _mm256_permute4x64_epi64(w_unorm.ireg,
							SWIZZLE(0, 2, 1, 3));
		__m256i mask = _mm256_permute4x64_epi64(p->t.mask_full,
							SWIZZLE(0, 2, 1, 3));
		_mm256_maskstore_epi32(base, mask, w_unorm.ireg);
	}

	return mask;
}

static void
dispatch_ps(struct payload *p, uint32_t mask, int x, int y)
{
	uint32_t g;

	if (!gt.ps.enable_simd8)
		return;

	assert(gt.ps.enable_simd8);

	/* Not sure what we should make this. */
	uint32_t fftid = 0;

	p->t.mask = mask;
	/* Fixed function header */
	p->t.grf[0] = (struct reg) {
		.ud = {
			/* R0.0 */
			gt.ia.topology |
			0 /*  FIXME: More here */,
			/* R0.1 */
			gt.cc.state,
			/* R0.2: MBZ */
			0,
			/* R0.3: per-thread scratch space, sampler ptr */
			gt.ps.sampler_state_address |
			gt.ps.scratch_size,
			/* R0.4: binding table pointer */
			gt.ps.binding_table_address,
			/* R0.5: fftid, scratch offset */
			gt.ps.scratch_pointer | fftid,
			/* R0.6: thread id */
			gt.ps.tid++ & 0xffffff,
			/* R0.7: Reserved */
			0,
		}
	};

	p->t.grf[1] = (struct reg) {
		.ud = {
			/* R1.0-1: MBZ */
			0,
			0,
			/* R1.2: x, y for subspan 0  */
			(y << 16) | x,
			/* R1.3: x, y for subspan 1  */
			(y << 16) | (x + 2),
			/* R1.4: x, y for subspan 2 (SIMD16) */
			0 | 0,
			/* R1.5: x, y for subspan 3 (SIMD16) */
			0 | 0,
			/* R1.6: MBZ */
			0 | 0,
			/* R1.7: Pixel sample mask and copy */
			mask | (mask << 16)

		}
	};

	g = 2;
	if (gt.wm.barycentric_mode & BIM_PERSPECTIVE_PIXEL) {
		p->t.grf[g].reg = p->w1.reg;
		p->t.grf[g + 1].reg = p->w2.reg;
		g += 2;
		/* if (simd16) ... */
	}

	if (gt.wm.barycentric_mode & BIM_PERSPECTIVE_CENTROID) {
		p->t.grf[g].reg = p->w1.reg;
		p->t.grf[g + 1].reg = p->w2.reg;
		g += 2;
		/* if (simd16) ... */
	}

	if (gt.wm.barycentric_mode & BIM_PERSPECTIVE_SAMPLE) {
		p->t.grf[g].reg = p->w1.reg;
		p->t.grf[g + 1].reg = p->w2.reg;
		g += 2;
		/* if (simd16) ... */
	}

	if (gt.wm.barycentric_mode & BIM_LINEAR_PIXEL) {
		g++; /* barycentric[1], slots 0-7 */
		g++; /* barycentric[2], slots 0-7 */
		/* if (simd16) ... */
	}

	if (gt.wm.barycentric_mode & BIM_LINEAR_CENTROID) {
		g++; /* barycentric[1], slots 0-7 */
		g++; /* barycentric[2], slots 0-7 */
		/* if (simd16) ... */
	}

	if (gt.wm.barycentric_mode & BIM_LINEAR_SAMPLE) {
		g++; /* barycentric[1], slots 0-7 */
		g++; /* barycentric[2], slots 0-7 */
		/* if (simd16) ... */
	}

	if (gt.ps.uses_source_depth) {
		g++; /* interpolated depth, slots 0-7 */
	}

	if (gt.ps.uses_source_w) {
		g++; /* interpolated w, slots 0-7 */
	}

	if (gt.ps.position_offset_xy == POSOFFSET_CENTROID) {
		g++;
	} else if (gt.ps.position_offset_xy == POSOFFSET_SAMPLE) {
		g++;
	}

	if (gt.ps.input_coverage_mask_state != ICMS_NONE) {
		g++;
	}

	if (gt.ps.push_constant_enable)
		g = load_constants(&p->t, &gt.ps.curbe, gt.ps.grf_start0);
	else
		g = gt.ps.grf_start0;

	if (gt.ps.attribute_enable) {
		memcpy(&p->t.grf[g], p->attribute_deltas,
		       gt.sbe.num_attributes * 2 * sizeof(p->t.grf[0]));
	}

	if (gt.ps.statistics)
		gt.ps_invocation_count++;

	dispatch_shader(gt.ps.avx_shader_simd8, &p->t);
}

const int tile_width = 512 / 4;
const int tile_height = 8;

struct tile_iterator {
	int x, y;
	__m256i row_w2, w2;
	__m256i row_w0, w0;
	__m256i row_w1, w1;
};

static void
tile_iterator_init(struct tile_iterator *iter, struct payload *p)
{
	__m256i w2_offsets, w0_offsets, w1_offsets;
	static const struct reg sx = { .d = {  0, 1, 0, 1, 2, 3, 2, 3 } };
	static const struct reg sy = { .d = {  0, 0, 1, 1, 0, 0, 1, 1 } };

	iter->x = 0;
	iter->y = 0;

	w2_offsets =
		_mm256_mullo_epi32(_mm256_set1_epi32(p->e01.a), sx.ireg) +
		_mm256_mullo_epi32(_mm256_set1_epi32(p->e01.b), sy.ireg);
	w0_offsets =
		_mm256_mullo_epi32(_mm256_set1_epi32(p->e12.a), sx.ireg) +
		_mm256_mullo_epi32(_mm256_set1_epi32(p->e12.b), sy.ireg);
	w1_offsets =
		_mm256_mullo_epi32(_mm256_set1_epi32(p->e20.a), sx.ireg) +
		_mm256_mullo_epi32(_mm256_set1_epi32(p->e20.b), sy.ireg);

	iter->row_w2 = _mm256_add_epi32(_mm256_set1_epi32(p->start_w2),
				       w2_offsets);

	iter->row_w0 = _mm256_add_epi32(_mm256_set1_epi32(p->start_w0),
				       w0_offsets);

	iter->row_w1 = _mm256_add_epi32(_mm256_set1_epi32(p->start_w1),
				       w1_offsets);

	iter->w2 = iter->row_w2;
	iter->w0 = iter->row_w0;
	iter->w1 = iter->row_w1;
}

static bool
tile_iterator_done(struct tile_iterator *iter)
{
	return iter->y == tile_height;
}

static void
tile_iterator_next(struct tile_iterator *iter, struct payload *p)
{
	iter->x += 4;
	if (iter->x == tile_width) {
		iter->x = 0;
		iter->y += 2;

		iter->row_w2 = _mm256_add_epi32(iter->row_w2, _mm256_set1_epi32(p->e01.b * 2));
		iter->row_w0 = _mm256_add_epi32(iter->row_w0, _mm256_set1_epi32(p->e12.b * 2));
		iter->row_w1 = _mm256_add_epi32(iter->row_w1, _mm256_set1_epi32(p->e20.b * 2));

		iter->w2 = iter->row_w2;
		iter->w0 = iter->row_w0;
		iter->w1 = iter->row_w1;

	} else {
		iter->w2 = _mm256_add_epi32(iter->w2, _mm256_set1_epi32(p->e01.a * 4));
		iter->w0 = _mm256_add_epi32(iter->w0, _mm256_set1_epi32(p->e12.a * 4));
		iter->w1 = _mm256_add_epi32(iter->w1, _mm256_set1_epi32(p->e20.a * 4));
	}

}

static void
compute_barycentric_coords(struct tile_iterator *iter, struct payload *p)
{
	/* We add back the tie-breaker adjustment so as to not distort
	 * the barycentric coordinates.*/
	p->w2.reg =
		_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(iter->w2, _mm256_set1_epi32(p->e01.bias))),
			      _mm256_set1_ps(p->inv_area));
	p->w0.reg =
		_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(iter->w0, _mm256_set1_epi32(p->e12.bias))),
			      _mm256_set1_ps(p->inv_area));
	p->w1.reg =
		_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(iter->w1, _mm256_set1_epi32(p->e20.bias))),
			      _mm256_set1_ps(p->inv_area));
}

static void
rasterize_rectlist_tile(struct payload *p)
{
}

static void
rasterize_triangle_tile(struct payload *p)
{
	struct tile_iterator iter;

	for (tile_iterator_init(&iter, p);
	     !tile_iterator_done(&iter);
	     tile_iterator_next(&iter, p)) {
		struct reg det;
		det.ireg =
			_mm256_and_si256(_mm256_and_si256(iter.w1,
							  iter.w0), iter.w2);

		/* Determine coverage: this is an e < 0 test,
		 * where we've subtracted 1 from top-left
		 * edges to include pixels on those edges. */
		uint32_t mask = _mm256_movemask_ps(det.reg);
		if (mask == 0)
			continue;

		/* Some pixels are covered and we have to
		 * calculate barycentric coordinates. */
		compute_barycentric_coords(&iter, p);

		p->t.mask_full = det.ireg;
		if (gt.depth.test_enable || gt.depth.write_enable)
			mask = depth_test(p, mask, p->x0 + iter.x, p->y0 + iter.y);

		if (mask && gt.ps.enable)
			dispatch_ps(p, mask, p->x0 + iter.x, p->y0 + iter.y);
	}
}

struct point {
	int32_t x, y;
};

static inline struct point
snap_point(float x, float y)
{
	return (struct point) {
		(int32_t) (x * 256.0f),
		(int32_t) (y * 256.0f)
	};
}

static inline void
init_edge(struct edge *e, struct point p0, struct point p1)
{
	e->a = (p0.y - p1.y);
	e->b = (p1.x - p0.x);
	e->c = ((int64_t) p1.y * p0.x - (int64_t) p1.x * p0.y) >> 8;
	e->bias = e->a < 0 || (e->a == 0 && e->b < 0);
	e->min_x = e->a > 0 ? 0 : 1;
	e->min_y = e->b > 0 ? 0 : 1;
}

static inline void
invert_edge(struct edge *e)
{
	e->a = -e->a;
	e->b = -e->b;
	e->c = -e->c;
	e->bias = 1 - e->bias;
}

static inline int
eval_edge(struct edge *e, struct point p)
{
	return (((int64_t) e->a * p.x + (int64_t) e->b * p.y) >> 8) + e->c - e->bias;
}

static void
bbox_iter_init(struct payload *p)
{
	p->x0 = p->min_x;
	p->y0 = p->min_y;

	p->start_w2 = p->row_w2;
	p->start_w0 = p->row_w0;
	p->start_w1 = p->row_w1;
}

static bool
bbox_iter_done(struct payload *p)
{
	return p->y0 == p->max_y;
}

static void
bbox_iter_next(struct payload *p)
{
	p->x0 += tile_width;
	if (p->x0 == p->max_x) {
		p->x0 = p->min_x;
		p->y0 += tile_height;
		p->row_w2 += tile_height * p->e01.b;
		p->row_w0 += tile_height * p->e12.b;
		p->row_w1 += tile_height * p->e20.b;
		p->start_w2 = p->row_w2;
		p->start_w0 = p->row_w0;
		p->start_w1 = p->row_w1;
	} else {
		p->start_w2 += tile_width * p->e01.a;
		p->start_w0 += tile_width * p->e12.a;
		p->start_w1 += tile_width * p->e20.a;
	}
}

void
rasterize_rectlist(struct payload *p)
{
	for (bbox_iter_init(p); !bbox_iter_done(p); bbox_iter_next(p))
		rasterize_rectlist_tile(p);
}


void
rasterize_triangle(struct payload *p)
{
	int min_w0_delta, min_w1_delta, min_w2_delta;

	const int tile_max_x = tile_width - 1;
	const int tile_max_y = tile_height - 1;

	/* delta from w in top-left corner to minimum w in tile */
	min_w2_delta = p->e01.a * p->e01.min_x * tile_max_x + p->e01.b * p->e01.min_y * tile_max_y;
	min_w0_delta = p->e12.a * p->e12.min_x * tile_max_x + p->e12.b * p->e12.min_y * tile_max_y;
	min_w1_delta = p->e20.a * p->e20.min_x * tile_max_x + p->e20.b * p->e20.min_y * tile_max_y;

	for (bbox_iter_init(p); !bbox_iter_done(p); bbox_iter_next(p)) {
		int32_t min_w2 = p->start_w2 + min_w2_delta;
		int32_t min_w0 = p->start_w0 + min_w0_delta;
		int32_t min_w1 = p->start_w1 + min_w1_delta;

		if ((min_w2 & min_w0 & min_w1) < 0)
			rasterize_triangle_tile(p);
	}
}

void
rasterize_primitive(struct primitive *prim)
{
	struct payload p;
	struct point p0 = snap_point(prim->v[0].x, prim->v[0].y);
	struct point p1 = snap_point(prim->v[1].x, prim->v[1].y);
	struct point p2 = snap_point(prim->v[2].x, prim->v[2].y);

	init_edge(&p.e01, p0, p1);
	init_edge(&p.e12, p1, p2);
	init_edge(&p.e20, p2, p0);
	p.area = eval_edge(&p.e01, p2);

	if ((gt.wm.front_winding == CounterClockwise &&
	     gt.wm.cull_mode == CULLMODE_FRONT) ||
	    (gt.wm.front_winding == Clockwise &&
	     gt.wm.cull_mode == CULLMODE_BACK) ||
	    (gt.wm.cull_mode == CULLMODE_NONE && p.area > 0)) {
		invert_edge(&p.e01);
		invert_edge(&p.e12);
		invert_edge(&p.e20);
		p.area = -p.area;
	}

	if (p.area >= 0)
		return;
	p.inv_area = 1.0f / p.area;

	float w[3] = {
		1.0f / prim->v[0].z,
		1.0f / prim->v[1].z,
		1.0f / prim->v[2].z
	};
	p.w_deltas[0] = w[1] - w[0];
	p.w_deltas[1] = w[2] - w[0];
	p.w_deltas[2] = 0.0f;
	p.w_deltas[3] = w[0];

	for (uint32_t i = 0; i < gt.sbe.num_attributes; i++) {
		const struct value a0 = prim->vue[0][i + 2];
		const struct value a1 = prim->vue[1][i + 2];
		const struct value a2 = prim->vue[2][i + 2];

		p.attribute_deltas[i * 2] = (struct reg) {
			.f = {
				a1.vec4.x - a0.vec4.x,
				a2.vec4.x - a0.vec4.x,
				0,
				a0.vec4.x,
				a1.vec4.y - a0.vec4.y,
				a2.vec4.y - a0.vec4.y,
				0,
				a0.vec4.y,
			}
		};
		p.attribute_deltas[i * 2 + 1] = (struct reg) {
			.f = {
				a1.vec4.z - a0.vec4.z,
				a2.vec4.z - a0.vec4.z,
				0,
				a0.vec4.z,
				a1.vec4.w - a0.vec4.w,
				a2.vec4.w - a0.vec4.w,
				0,
				a0.vec4.w,
			}
		};
	}

	static const struct reg sx = { .d = {  0, 1, 0, 1, 2, 3, 2, 3 } };
	static const struct reg sy = { .d = {  0, 0, 1, 1, 0, 0, 1, 1 } };

	if (gt.depth.write_enable || gt.depth.test_enable) {
		uint64_t range;
		uint32_t cpp = depth_format_size(gt.depth.format);

		p.depth.offsets.ireg =
			_mm256_add_epi32(_mm256_mullo_epi32(sx.ireg, _mm256_set1_epi32(cpp)),
					 _mm256_mullo_epi32(sy.ireg, _mm256_set1_epi32(gt.depth.stride)));
		p.depth.buffer = map_gtt_offset(gt.depth.address, &range);
	}

	p.min_x = INT_MAX;
	p.min_y = INT_MAX;
	p.max_x = INT_MIN;
	p.max_y = INT_MIN;
	for (int i = 0; i < 3; i++) {
		int x, y;

		x = floor(prim->v[i].x);
		if (x < p.min_x)
			p.min_x = x;
		y = floor(prim->v[i].y);
		if (y < p.min_y)
			p.min_y = y;

		x = ceil(prim->v[i].x);
		if (p.max_x < x)
			p.max_x = x;
		y = ceil(prim->v[i].y);
		if (p.max_y < y)
			p.max_y = y;
	}

	if (p.min_x < gt.drawing_rectangle.min_x)
		p.min_x = gt.drawing_rectangle.min_x;
	if (p.min_y < gt.drawing_rectangle.min_y)
		p.min_y = gt.drawing_rectangle.min_y;
	if (p.max_x > gt.drawing_rectangle.max_x)
		p.max_x = gt.drawing_rectangle.max_x;
	if (p.max_y > gt.drawing_rectangle.max_y)
		p.max_y = gt.drawing_rectangle.max_y;

	p.min_x = p.min_x & ~(tile_width - 1);
	p.min_y = p.min_y & ~(tile_height - 1);
	p.max_x = (p.max_x + tile_width - 1) & ~(tile_width - 1);
	p.max_y = (p.max_y + tile_height - 1) & ~(tile_height - 1);

	if (p.max_x <= p.min_x || p.max_y < p.min_y)
		return;

	struct point min = snap_point(p.min_x, p.min_y);
	min.x += 128;
	min.y += 128;
	p.row_w2 = eval_edge(&p.e01, min);
	p.row_w0 = eval_edge(&p.e12, min);
	p.row_w1 = eval_edge(&p.e20, min);

	if (gt.ia.topology == _3DPRIM_RECTLIST)
		rasterize_rectlist(&p);
	else
		rasterize_triangle(&p);
}

void
wm_flush(void)
{
	struct surface rt;

	if (framebuffer_filename &&
	    get_surface(gt.ps.binding_table_address, 0, &rt))
		write_png(framebuffer_filename,
			  rt.width, rt.height, rt.stride, rt.pixels);
}

void
wm_clear(void)
{
	struct surface rt;
	void *depth;
	uint64_t range;

	if (gt.ps.resolve_type == RESOLVE_DISABLED &&
	    get_surface(gt.ps.binding_table_address, 0, &rt)) {
		int height = (rt.height + 7) & ~7;
		memset(rt.pixels, 0, height * rt.stride);
		if (gt.depth.write_enable) {
			depth = map_gtt_offset(gt.depth.address, &range);
			height = (gt.depth.height + 31) & ~31;
			memset(depth, 0, gt.depth.stride * height);
		}
	}
}

void
hiz_clear(void)
{
	uint64_t range;
	void *depth;

	if (!gt.depth.write_enable)
		return;

	depth = map_gtt_offset(gt.depth.address, &range);
	int height = (gt.depth.height + 31) & ~31;

	memset(depth, 0, gt.depth.stride * height);
}
