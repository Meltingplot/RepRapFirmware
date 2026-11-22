// The names that alloc-check.py knows, for the selftest: the region primitives of FreeRTOS, the heap functions of
// RepRapFirmware's src/libc/nano-mallocr.c, and the instructions that CoreN2G's Core.h and CoreIO.h use to mask interrupts.
// runtime.cpp defines the functions; they do nothing useful, the selftest only reads their machine code.

#ifndef ALLOC_CHECK_SELFTEST_RUNTIME_H
#define ALLOC_CHECK_SELFTEST_RUNTIME_H

#include <cstddef>
#include <cstdint>

extern "C"
{
	void vTaskSuspendAll() noexcept;
	long xTaskResumeAll() noexcept;
	void vPortEnterCritical() noexcept;
	void vPortExitCritical() noexcept;
	void *malloc(size_t n) noexcept;
	void free(void *p) noexcept;
}

static inline __attribute__((always_inline)) uint32_t GetPrimask() noexcept
{
	uint32_t r;
	__asm volatile ("mrs %0, primask" : "=r" (r) :: "memory");
	return r;
}

static inline __attribute__((always_inline)) void DisableIrq() noexcept { __asm volatile ("cpsid i" ::: "memory"); }
static inline __attribute__((always_inline)) void SetPrimask(uint32_t v) noexcept { __asm volatile ("msr primask, %0" :: "r" (v) : "memory"); }

static inline __attribute__((always_inline)) uint32_t GetBasepri() noexcept
{
	uint32_t r;
	__asm volatile ("mrs %0, basepri" : "=r" (r) :: "memory");
	return r;
}

static inline __attribute__((always_inline)) void SetBasepriMax(uint32_t v) noexcept { __asm volatile ("msr basepri_max, %0" :: "r" (v) : "memory"); }
static inline __attribute__((always_inline)) void SetBasepri(uint32_t v) noexcept { __asm volatile ("msr basepri, %0" :: "r" (v) : "memory"); }

#endif
