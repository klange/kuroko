#pragma once
/**
 * @file kuroko.h
 * @brief Top-level header with configuration macros.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__EMSCRIPTEN__)
typedef int krk_integer_type;
# define PRIkrk_int "%d"
# define PRIkrk_hex "%x"
# define parseStrInt strtol
#elif defined(_WIN32)
typedef int krk_integer_type;
# define PRIkrk_int "%I32d"
# define PRIkrk_hex "%I32x"
# define parseStrInt strtol
# define ENABLE_THREADING
# else
typedef int krk_integer_type;
# define PRIkrk_int "%d"
# define PRIkrk_hex "%x"
# define parseStrInt strtol
# define ENABLE_THREADING
#endif

#ifdef DEBUG
#define ENABLE_DISASSEMBLY
#define ENABLE_TRACING
#define ENABLE_SCAN_TRACING
#define ENABLE_STRESS_GC
#endif

#ifndef _WIN32
# ifndef STATIC_ONLY
#  include <dlfcn.h>
# endif
# define PATH_SEP "/"
# define dlRefType void *
# define dlSymType void *
# define dlOpen(fileName) dlopen(fileName, RTLD_NOW)
# define dlSym(dlRef, handlerName) dlsym(dlRef,handlerName)
# define dlClose(dlRef) dlclose(dlRef)
#else
# include <windows.h>
# define PATH_SEP "\\"
# define dlRefType HINSTANCE
# define dlSymType FARPROC
# define dlOpen(fileName) LoadLibraryA(fileName)
# define dlSym(dlRef, handlerName) GetProcAddress(dlRef, handlerName)
# define dlClose(dlRef)
#endif

