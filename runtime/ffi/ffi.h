/* rsharp/runtime/ffi/ffi.h — C Foreign Function Interface helpers */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* C calling convention ABI helpers */
typedef struct RsFfiType {
    enum { FFI_VOID, FFI_INT, FFI_FLOAT, FFI_PTR, FFI_STRUCT } kind;
    int   width;    /* bits */
    bool  is_signed;
} RsFfiType;

typedef struct RsFfiArg {
    RsFfiType type;
    union { int64_t i; double f; void *p; } value;
} RsFfiArg;

/* Dynamically call a C function by pointer */
typedef void (*RsFnPtr)(void);
int  rsffi_call(RsFnPtr fn, RsFfiArg *args, size_t argc, RsFfiArg *ret);

/* Platform-specific dynamic library loading */
void *rsffi_dlopen(const char *path);
void *rsffi_dlsym(void *lib, const char *name);
void  rsffi_dlclose(void *lib);
