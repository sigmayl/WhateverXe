#pragma once
// Minimal <stdlib.h> for the cross-built shim. The symbols are provided by
// libSystem at load time; the declarations keep the host SDK out of the build.

#include <stddef.h>

extern "C" void *malloc(size_t size) noexcept;
extern "C" void *calloc(size_t count, size_t size) noexcept;
extern "C" void *realloc(void *pointer, size_t size) noexcept;
extern "C" void free(void *pointer) noexcept;
extern "C" void abort(void) noexcept;
extern "C" int abs(int value) noexcept;
