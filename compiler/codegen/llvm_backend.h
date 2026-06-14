// compiler/codegen/llvm_backend.h — R# LLVM Backend API
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "../parser/ast.h"
#include "../error/diagnostic.h"
#include "../../runtime/memory/arena.h"
typedef enum OptLevel { OPT_DEBUG=0, OPT_RELEASE_SAFE=1, OPT_RELEASE_FAST=2, OPT_SIZE=3 } OptLevel;
void       *llvm_backend_create(DiagSink *diags, int opt_level, const char *triple, const char *cpu, int emit_ir);
int         llvm_backend_compile(void *backend, const AstFile *ast, const char *out_path);
const char *llvm_backend_emit_ir(void *backend, const AstFile *ast, Arena *arena);
void        llvm_backend_destroy(void *backend);
#ifdef __cplusplus
}
#endif
