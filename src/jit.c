
/* This file is part of minemu
 *
 * Copyright 2010-2011 Erik Bosman <erik@minemu.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <string.h>
#include <limits.h>

#include "lib.h"
#include "mm.h"

#include "jit.h"
#include "jit_mm.h"
#include "codemap.h"
#include "jmp_cache.h"
#include "opcodes.h"
#include "jit_code.h"
#include "error.h"
#include "runtime.h"
#include "debug.h"
#include "syscalls.h"
#include "jit_cache.h"
#include "threads.h"

long jit_lock = 0;

#define TRANSLATED_MAX_SIZE (255)

unsigned long min(unsigned long a, unsigned long b) { return a<b ? a:b; }

/* jit code block layout:
 * (allocated in the jit code section of the address space)
 *
 * offset
 * -----------------------
 * 0x00                chunk header
 * sizeof(jit_chunk_t) jit code
 * hdr.tbl_off         translated instruction sizes
 *                     (a pair (orig size, jit size) of bytes per instruction)
 * hdr.chunk_len-2     terminating with ( 0, ... )
 * -----------------------
 * hdr.chunk_len       next chunk
 * ....
 * -----------------------
 * map.jit_len         end
 *
 */

/* We make sure that no fields in our jit code-structures use
 * absolute pointers into themselves. This way we can move pieces
 * of code more easily. (We only have to update the jump cache.)
 */

typedef struct
{
	char *addr; unsigned long len;
	unsigned long chunk_len, lookup_off, tbl_off, n_ops;
	int tree_depth;

} jit_chunk_t;


/* a */
typedef struct
{
	unsigned char orig, jit;

} size_pair_t;

typedef struct
{
	char *addr;
	unsigned long off;

} rel_jmp_t;

/* Small min-heap implementation for relative jumps
 *
 * adding an element to the heap -> log n
 * removing the smallest element -> log n
 * with n relative to the number of jumps in the list
 *
 * During compilation all jumps are added and removed exaclty once,
 * which makes the worst case complexity n log n relative to the
 * number of jumps in the code.
 *
 */

typedef struct
{
	rel_jmp_t *buf;
	unsigned long size, max_size;

} jmp_heap_t;

/* create new empty heap using buffer buf */
void heap_init(jmp_heap_t *h, rel_jmp_t *buf, unsigned long buf_len)
{
	*h = (jmp_heap_t)
	{
		.buf = buf,
		.size = 0,
		.max_size = buf_len,
	};
}

/* puts the contents of *jmp in the heap */
void heap_put(jmp_heap_t *h, rel_jmp_t *jmp)
{
	if (h->size >= h->max_size)
		die("minemu: heap overflow");

	unsigned long i = h->size, parent = (i+1)/2-1;
	rel_jmp_t tmp;

	h->buf[i] = *jmp;
	h->size++;

	/* sift up */
	while ( (i > 0) &&
	        (unsigned long)h->buf[i].addr <
	        (unsigned long)h->buf[parent].addr )
	{
		tmp = h->buf[i];
		h->buf[i] = h->buf[parent];
		h->buf[parent] = tmp;

		i = parent;
		parent = (i+1)/2-1;
	}
}

/* Copies the relative jump with the smallest original address to *jmp,
 * removes it from the heap.
 *
 * Returns true unless the heap is empty in which case it returns false
 * and nothing is copied.
 */
int heap_get(jmp_heap_t *h, rel_jmp_t *jmp)
{
	if (h->size == 0)
		return 0;

	unsigned long i = 0, child = (i+1)*2-1;
	rel_jmp_t tmp;

	*jmp = h->buf[0];
	h->size--;
	h->buf[0] = h->buf[h->size];

	/* sift down */
	while (child < h->size)
	{
		/* choose child no. 2 if it has a lower address */
		if ( (child+1 < h->size) &&
		     ((unsigned long)h->buf[child+1].addr <
		      (unsigned long)h->buf[child].addr) )
			child+=1;

		if ( (unsigned long)h->buf[i].addr <
		     (unsigned long)h->buf[child].addr )
			break;

		tmp = h->buf[i];
		h->buf[i] = h->buf[child];
		h->buf[child] = tmp;

		i = child;
		child = (i+1)*2-1;
	}

	return 1;
}

/* address lookup
 * also called by runtime_ijmp in case of a cache miss (on a small stack)
 */

#define FRAME_SHIFT (8)
#define FRAME_SIZE ( 1<<(FRAME_SHIFT) )

typedef struct
{
	unsigned long d_off;
	unsigned long tbl_off;

} jit_lookup_t;

#define ALIGN(x, n) ( ( (long)(x)+(n)-1 ) & ~( (n) -1 ) )
#define CHUNK_OFFSET(x) ( (long)(x) - (long)(hdr) )
#define DIV_CEIL(x, d) ( ( (long)(x)+(long)(d)-1)/(long)(d) )

static void jit_chunk_create_lookup_mapping(jit_chunk_t *hdr, size_pair_t *sizes, char *jit_end)
{
	long n_frames = DIV_CEIL(hdr->len, FRAME_SIZE);

	jit_lookup_t *lookup = (jit_lookup_t *)ALIGN((long)hdr+hdr->chunk_len, 64);
	hdr->lookup_off = CHUNK_OFFSET(lookup);

	size_pair_t *table = (size_pair_t *)&lookup[n_frames];
	hdr->tbl_off = CHUNK_OFFSET(table);

	unsigned long n_ops = hdr->n_ops, i, j, cur_lookup = 0,
	              s_off = 0, d_off = sizeof(*hdr);

	for (i=0, j=0; i < n_ops; i++, j++)
	{
		if ( ( (s_off-1) ^ (s_off+sizes[i].orig-1) ) & ~(FRAME_SIZE-1) ) 
		{
			lookup[cur_lookup] = (jit_lookup_t) { .d_off = d_off, .tbl_off = CHUNK_OFFSET(&table[j]) };
			cur_lookup++;

			if ( s_off & (FRAME_SIZE-1) ) /* we use orig == 0 to encode that jit contains */
			{                             /* a negative s_off in case frame boundaries fall  */
			                              /* between instructions                            */
				table[j] = (size_pair_t) { .orig = 0, .jit = -s_off & (FRAME_SIZE-1) };
				j++;
			}
		}

		if ((unsigned long)&table[j+1] > (unsigned long)jit_end)
			die("jit chunk too large");

		table[j] = sizes[i];
		s_off   += sizes[i].orig;
		d_off   += sizes[i].jit;
	}

	hdr->chunk_len = CHUNK_OFFSET(ALIGN(&table[j], 64));
}

#undef ALIGN

static char *jit_chunk_lookup_addr(jit_chunk_t *hdr, char *addr)
{
	if (!contains(hdr->addr, hdr->len, addr))
		return NULL;

	jit_lookup_t *lookup = (jit_lookup_t *)((long)hdr+hdr->lookup_off);
	jit_lookup_t *frame_entry = &lookup[ ((long)addr-(long)hdr->addr) >> FRAME_SHIFT ];
	size_pair_t *sizes = (size_pair_t *)((long)hdr+frame_entry->tbl_off);
	unsigned long s_off = ((long)addr-(long)hdr->addr) & ~(FRAME_SIZE-1),
	              d_off = frame_entry->d_off,
	              i;

	i=0;
	if (sizes[i].orig == 0)
	{
		s_off -= sizes[i].jit;
		i++;
	}

	while ((unsigned long)addr >= (unsigned long)&hdr->addr[s_off])
	{
		if ((unsigned long)addr == (unsigned long)&hdr->addr[s_off])
			return (char *)hdr+d_off;

		s_off += sizes[i].orig;
		d_off += sizes[i].jit;
		i++;
	}

	return NULL;
}

static char *jit_map_lookup_addr(code_map_t *map, char *addr)
{
	unsigned long off = 0;
	char *jit_addr;

	if (map->jit_addr == NULL)
		return NULL;

	while (off < map->jit_len)
	{
		jit_chunk_t *hdr = (jit_chunk_t *)&map->jit_addr[off];

		if ( (jit_addr = jit_chunk_lookup_addr(hdr, addr)) )
			return jit_addr;

		off += hdr->chunk_len;
	}

	return NULL;
}

char *jit_lookup_addr(char *addr)
{
	code_map_t *map = find_code_map(addr);
	char *jit_addr = NULL;

	if (map)
		jit_addr = jit_map_lookup_addr(map, addr);

	if (jit_addr)
		add_jmp_mapping(addr, jit_addr);

	return jit_addr;
}

/* reverse address lookup */

static char *jit_chunk_rev_lookup_addr(jit_chunk_t *hdr, char *jit_addr, char **jit_op_start, long *jit_op_len)
{
	if (!contains((char *)hdr, hdr->lookup_off, jit_addr))
		return NULL;

	long n_frames = DIV_CEIL(hdr->len, FRAME_SIZE), mid;
	jit_lookup_t *lookup = (jit_lookup_t *)((long)hdr+hdr->lookup_off);
	unsigned long in_d_off = CHUNK_OFFSET(jit_addr), d_off, s_off = 0, i;

	while (n_frames > 1)
	{
		mid = n_frames/2;
		d_off = lookup[mid].d_off;
	
		if ( in_d_off < d_off )
		{
			n_frames = mid;
		}
		else
		{
			s_off += mid*FRAME_SIZE;
			lookup = &lookup[mid];
			n_frames -= mid;
		}
	}

	d_off = lookup->d_off;

	if ( in_d_off < d_off )
		return NULL;

	size_pair_t *sizes = (size_pair_t *)((long)hdr+lookup->tbl_off);

	i=0;
	if (sizes[i].orig == 0)
	{
		s_off -= sizes[i].jit;
		i++;
	}

	while ( (unsigned long)(d_off+sizes[i].jit) <= in_d_off )
	{
		s_off += sizes[i].orig;
		d_off += sizes[i].jit;
		i++;
	}

	if (jit_op_start)
		*jit_op_start = &((char *)hdr)[d_off];
	if (jit_op_len)
		*jit_op_len = sizes[i].jit;

	return &hdr->addr[s_off];
}

static char *jit_map_rev_lookup_addr(code_map_t *map, char *jit_addr, char **jit_op_start, long *jit_op_len)
{
	unsigned long off = 0;
	char *addr;

	while (off < map->jit_len)
	{
		jit_chunk_t *hdr = (jit_chunk_t *)&map->jit_addr[off];

		if ( (addr = jit_chunk_rev_lookup_addr(hdr, jit_addr, jit_op_start, jit_op_len)) )
			return addr;

		off += hdr->chunk_len;
	}

	return NULL;
}

char *jit_rev_lookup_addr(char *jit_addr, char **jit_op_start, long *jit_op_len)
{
	code_map_t *map = find_jit_code_map(jit_addr);

	if (map)
		return jit_map_rev_lookup_addr(map, jit_addr, jit_op_start, jit_op_len);

	return NULL;
}

/*  */

static void jit_chunk_fill_mapping(code_map_t *map, jit_chunk_t *hdr,
                                   unsigned long *mapping)
{
	unsigned long n_ops = hdr->n_ops,
	              i,j,
	              jit_off = (char *)&hdr[1] - map->jit_addr,
	              orig_off = hdr->addr - map->addr;

	size_pair_t *sizes = (size_pair_t *)&((char *)hdr)[hdr->tbl_off];

	for (i=0,j=0; i<n_ops; i++,j++)
	{
		if ( sizes[j].orig == 0 )
			j++;

		mapping[orig_off] = jit_off;
		orig_off += sizes[j].orig;
		jit_off += sizes[j].jit;
	}
}

static void jit_fill_mapping(code_map_t *map, unsigned long *mapping,
                                              unsigned long mapsize)
{
	unsigned long off = 0;
	memset(mapping, 0, mapsize*sizeof(char *));

	while (off < map->jit_len)
	{
		jit_chunk_t *hdr = (jit_chunk_t *)&map->jit_addr[off];
		jit_chunk_fill_mapping(map, hdr, mapping);
		off += hdr->chunk_len;
	}
}

static void jit_map_resize(code_map_t *map, unsigned long new_len)
{
	if ( new_len > jit_size(map->jit_addr) )
		die("jmp mapping resize is not supported");

	commit();
	map->jit_len = new_len;
	commit();
}

#define TRANSLATED(m) ((unsigned long)(m+0x1000)>0x1000)
/*#define UNTRANSLATED  ((unsigned long) 0)*/
#define NEEDED        ((unsigned long)-1)

static int try_resolve_jmp(code_map_t *map, char *jmp_addr, char *imm_addr,
                           unsigned long *mapping)
{
	if ( TRANSLATED(mapping[jmp_addr-map->addr]) )
	{
		long diff = (long)&map->jit_addr[mapping[jmp_addr-map->addr]] -
		            (long)imm_addr - 4;
		imm_to(imm_addr, diff);
		return 1;
	}
	else
		return 0;
}

/* Translate a chunk of chunk of code
 *
 */
static jit_chunk_t *jit_translate_chunk(code_map_t *map, char *entry_addr,
                                        jmp_heap_t *jmp_heap, unsigned long *mapping)
{
	char *jit_addr=map->jit_addr, *addr=map->addr;
	unsigned long n_ops = 0,
	              entry = entry_addr-addr,
	              s_off = entry_addr-addr,
	              d_off = map->jit_len+sizeof(jit_chunk_t),
	              chunk_base = map->jit_len,
	              max_len = jit_size(jit_addr);
	int stop = 0;

	instr_t instr;
	trans_t trans;
	rel_jmp_t jmp;
	size_pair_t sizes[map->len];

	while (stop == 0)
	{
		if ( d_off+TRANSLATED_MAX_SIZE > max_len )
			die("jmp mapping too big, additional allocation not implemented");

		mapping[s_off] = d_off;
		stop = read_op(&addr[s_off], &instr, map->len-s_off);
		translate_op(&jit_addr[d_off], &instr, &trans, map->addr, map->len);

		/* try to resolve translated jumps early */
		if ( (trans.imm != 0) && !try_resolve_jmp(map, trans.jmp_addr,
		                                          &jit_addr[d_off+trans.imm],
		                                          mapping) )
		{
			/* destination address not translated yet */
			jmp = (rel_jmp_t){ .addr=trans.jmp_addr, .off=d_off+trans.imm };
			heap_put(jmp_heap, &jmp);

			if (TRANSLATED(mapping[trans.jmp_addr-map->addr]))
				die("minemu bug");
			/* mark address as destination of jump
			 * to know whether to stop translation of code after
			 * unconditional jumps & returns
			 */
			mapping[trans.jmp_addr-map->addr] = NEEDED;
		}

		d_off += trans.len;
		s_off += instr.len;
		sizes[n_ops] = (size_pair_t) { instr.len, trans.len };

		if ( TRANSLATED(mapping[s_off]) )
		{
			stop = 1;
			generate_jump(&jit_addr[d_off], &addr[s_off], &trans,
			              map->addr, map->len);

			if (trans.imm != 0)
				if (!try_resolve_jmp(map, trans.jmp_addr,
				                     &jit_addr[d_off+trans.imm], mapping))
					die("minemu: assertion failed in jit_translate_chunk()");

			d_off += trans.len;
		}

		n_ops++;
	}

	jit_chunk_t *hdr = (jit_chunk_t*)&jit_addr[chunk_base];
	*hdr = (jit_chunk_t)
	{
		.addr = &addr[entry],
		.len = s_off-entry,
		.chunk_len = d_off-chunk_base, /* to be extrended during lookup map creation */
		.n_ops = n_ops,
	};

	jit_chunk_create_lookup_mapping(hdr, sizes, &jit_addr[max_len]);
	jit_map_resize(map, chunk_base+hdr->chunk_len);

	return hdr;
}

/* Translates all reachable code from map starting from entry_addr
 *
 */
static void jit_translate(code_map_t *map, char *entry_addr)
{
	jmp_heap_t jmp_heap;
	rel_jmp_t j;
	rel_jmp_t jumps[map->len/8]; /* mostly unused */
	unsigned long mapping[map->len+1]; /* waste of memory :-( */

	heap_init(&jmp_heap, jumps, map->len/8);

	jit_fill_mapping(map, mapping, map->len+1);

	char *base = (char *)PAGE_BASE(&map->jit_addr[map->jit_len]);
	unsigned long len = PAGE_NEXT(jit_size(map->jit_addr)-map->jit_len);
	sys_mprotect(base, len, PROT_READ|PROT_WRITE|PROT_EXEC);

	jit_translate_chunk(map, entry_addr, &jmp_heap, mapping);

	while (heap_get(&jmp_heap, &j))
		while (!try_resolve_jmp(map, j.addr, &map->jit_addr[j.off], mapping))
			jit_translate_chunk(map, j.addr, &jmp_heap, mapping);

	sys_mprotect(base, len, PROT_READ|PROT_EXEC);
}

void jit_init(void)
{
	jit_mm_init();
}

char *jit(char *addr)
{
	char *jit_addr = find_jmp_mapping(addr);

	if (jit_addr != NULL)
		return jit_addr;

	code_map_t *map = find_code_map(addr);

	if (map && map->jit_addr == NULL)
	{
		map->jit_addr = jit_alloc(map->len*5);

		try_load_jit_cache(map);
	}

	if (map == NULL)
		die("attempting to jump in non-executable code addr: %X ", addr);

	jit_addr = jit_lookup_addr(addr);

	if (jit_addr == NULL)
	{
		jit_translate(map, addr);
		jit_addr = jit_lookup_addr(addr);

		try_save_jit_cache(map);
	}

	if (jit_addr == NULL)
		die("jit failed");

	return jit_addr;
}

