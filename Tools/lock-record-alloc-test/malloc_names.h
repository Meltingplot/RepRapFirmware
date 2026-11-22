/* Included in front of RRF's src/libc/nano-mallocr.c (gcc -include). It renames the functions and globals that the file defines, so
 * that its malloc runs on the private arena of arena.cpp next to the host's own malloc, and declares the preemption hook that
 * gen/nano-mallocr-hook326.c calls. The text of nano-mallocr.c itself is unchanged (the hook build inserts one call, see the Makefile). */

/* nano-mallocr.c:88 uses ptrdiff_t and :814 struct _reent; on the target both come in through newlib's headers, the host's do not provide them
 * via eCvInclude/malloc.h */
#include <stddef.h>
struct _reent;

#define malloc					test_malloc
#define free					test_free
#define calloc					test_calloc
#define cfree					test_cfree
#define memalign				test_memalign
#define mallinfo				test_mallinfo
#define _sbrk					test_sbrk
#define __malloc_free_list		test_malloc_free_list
#define __malloc_sbrk_start		test_malloc_sbrk_start
#define __malloc_current_mallinfo	test_malloc_current_mallinfo

void TestPreemptionPointSplit(void);
