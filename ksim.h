#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>

static inline void
__ksim_assert(int cond, const char *file, int line, const char *msg)
{
	if (!cond) {
		printf("%s:%d: assert failed: %s\n", file, line, msg);
		raise(SIGTRAP);
	}			
}

#define ksim_assert(cond) __ksim_assert((cond), __FILE__, __LINE__, #cond)

static inline void
ksim_warn(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vfprintf(stdout, fmt, va);
	va_end(va);
}

static inline bool
is_power_of_two(uint64_t v)
{
	return (v & (v - 1)) == 0;
}

static inline uint64_t
align_u64(uint64_t v, uint64_t a)
{
	ksim_assert(is_power_of_two(a));

	return (v + a - 1) & ~(a - 1);
}

static inline uint64_t
max_u64(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

void start_batch_buffer(uint32_t *p);

enum {
	KSIM_VERTEX_STAGE,
	KSIM_GEOMETRY_STAGE,
	KSIM_HULL_STAGE,
	KSIM_DOMAIN_STAGE,
	KSIM_FRAGMENT_STAGE,
	KSIM_COMPUTE_STAGE,
	NUM_STAGES
};

/* bdw gt3 */
#define URB_SIZE (384 * 1024)

/* Per stage urb allocation info and entry pool. All sizes in bytes */
struct urb {
	uint32_t size;
	uint32_t count, total, free_list;
	void *data;
};

struct curbe {
	uint32_t size;
	struct {
		uint32_t length;
		uint64_t address;
	} buffer[4];
};

struct gt {
	struct {
		struct vb {
			uint64_t address;
			uint32_t size;
			uint32_t pitch;
		} vb[32];
		uint32_t vb_valid;
		struct ve {
			uint32_t vb;
			bool valid;
			uint32_t format;
			bool edgeflag;
			uint32_t offset;
			uint8_t cc[4];

			bool instancing;
			uint32_t step_rate;
		} ve[33];
		uint32_t ve_count;
		struct {
			uint32_t format;
			uint64_t address;
			uint32_t size;
		} ib;

		bool iid_enable;
		uint32_t iid_element;
		uint32_t iid_component;
		bool vid_enable;
		uint32_t vid_element;
		uint32_t vid_component;
		uint32_t topology;
		bool statistics;
		uint32_t cut_index;
	} vf;

	struct {
		bool enable;
		bool simd8;
		bool statistics;
		uint32_t vue_read_length;
		uint32_t vue_read_offset;
		uint64_t ksp;
		uint32_t urb_start_grf;
		struct urb urb;
		struct curbe curbe;
		uint32_t binding_table_address;
		uint32_t sampler_state_address;
	} vs;

	struct {
		struct urb urb;
		struct curbe curbe;
		uint32_t binding_table_address;
		uint32_t sampler_state_address;
	} hs;

	struct {
		struct urb urb;
		struct curbe curbe;
		uint32_t binding_table_address;
		uint32_t sampler_state_address;
	} ds;

	struct {
		struct urb urb;
		struct curbe curbe;
		uint32_t binding_table_address;
		uint32_t sampler_state_address;
	} gs;

	struct {
		struct curbe curbe;
		uint32_t binding_table_address;
		uint32_t sampler_state_address;
	} ps;

	char urb[URB_SIZE];

	bool curbe_dynamic_state_base;

	uint64_t general_state_base_address;
	uint64_t surface_state_base_address;
	uint64_t dynamic_state_base_address;
	uint64_t indirect_object_base_address;
	uint64_t instruction_base_address;

	uint32_t general_state_buffer_size;
	uint32_t dynamic_state_buffer_size;
	uint32_t indirect_object_buffer_size;
	uint32_t general_instruction_size;

	uint64_t sip_address;

	struct {
		bool predicate;
		bool end_offset;
		uint32_t access_type;
		
		uint32_t vertex_count;
		uint32_t start_vertex;
		uint32_t instance_count;
		uint32_t start_instance;
		int32_t base_vertex;
	} prim;

	struct {
		uint32_t dimx;
		uint32_t dimy;
		uint32_t dimz;
	} dispatch;

	uint32_t vs_invocation_count;
	uint32_t ia_vertices_count;
	uint32_t ia_primitives_count;
};

extern struct gt gt;

void *map_gtt_offset(uint64_t offset, uint64_t *range);

#define for_each_bit(b, dword)                          \
   for (uint32_t __dword = (dword);                     \
        (b) = __builtin_ffs(__dword) - 1, __dword;      \
        __dword &= ~(1 << (b)))

struct reg {
	union {
		float f[8];
		uint32_t u[8];
		int32_t d[8];
	};
};

struct thread {
	struct reg grf[128];
};

void run_thread(struct thread *t);

struct value {
	union {
		struct { float x, y, w, z; } vec4;
		struct { int32_t x, y, w, z; } ivec4;
		int32_t v[4];
	};
};

static inline struct value
vec4(float x, float y, float z, float w)
{
	return (struct value) { .vec4 = { x, y, z, w } };
}

static inline struct value
ivec4(int32_t x, int32_t y, int32_t z, int32_t w)
{
	return (struct value) { .ivec4 = { x, y, z, w } };
}

void dispatch_primitive(void);

bool valid_vertex_format(uint32_t format);
uint32_t format_size(uint32_t format);
struct value fetch_format(uint64_t offset, uint32_t format);


#define __gen_address_type uint32_t
#define __gen_combine_address(data, dst, address, delta) delta
#define __gen_user_data void

#include "gen8_pack.h"
