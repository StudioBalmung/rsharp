// rsharp/compiler/codegen/llvm_backend.cpp
// R# → LLVM IR Code Generator
// Language: C++20
//
// This is the primary compilation backend. Takes a type-checked AST
// (from the C sema pass) and emits LLVM IR, which is then compiled
// to native code via llc + lld (or directly via MCJIT for `rsrun`).
//
// Requires: LLVM 17+

#pragma once

// Standard includes only — actual LLVM headers pulled in via build system
// (avoids polluting the header with LLVM's massive include tree)
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <optional>
#include <cstdint>

// Forward-declared C AST types (linked from parser/ast.h via extern "C")
extern "C" {
#include "../parser/ast.h"
#include "../error/diagnostic.h"
}

namespace rsharp {
namespace codegen {

// ─── LLVM opaque wrappers (actual types in .cpp to avoid header pollution) ──

struct LLVMState;

// ─── Code generation options ─────────────────────────────────────────────────

enum class OptLevel { Debug, ReleaseSafe, ReleaseFast, ReleaseSmall };
enum class TargetOS { Native, Linux, Windows, Wasm };

struct CodegenOptions {
    OptLevel   opt      = OptLevel::Debug;
    TargetOS   target   = TargetOS::Native;
    bool       emit_ir  = false;   // write .ll alongside .o
    bool       lto      = false;
    std::string triple  = "";      // empty = host triple
    std::string cpu     = "generic";
    std::string features = "";
};

// ─── Value representation ─────────────────────────────────────────────────────
// We wrap LLVM Value* in a thin struct to avoid exposing LLVM headers here.

struct IRValue {
    void *llvm_value;  // really llvm::Value*
    bool  is_lvalue;   // true → needs a load before use
};

// ─── Codegen context ──────────────────────────────────────────────────────────

class LLVMBackend {
public:
    explicit LLVMBackend(DiagSink *diags, CodegenOptions opts);
    ~LLVMBackend();

    // Non-copyable, movable
    LLVMBackend(const LLVMBackend&)            = delete;
    LLVMBackend& operator=(const LLVMBackend&) = delete;
    LLVMBackend(LLVMBackend&&)                 = default;

    // ── Main entry points ─────────────────────────────────────────────────────

    /// Compile a full AstFile to an object file (.o)
    bool compile_file(const AstFile *ast, const std::string &out_path);

    /// Emit LLVM IR text to a string (for debugging / --emit-ir)
    std::string emit_ir(const AstFile *ast);

    /// JIT-compile and run main() — used by `rsrun`
    int jit_run(const AstFile *ast, int argc, char **argv);

    // ── Declaration codegen ───────────────────────────────────────────────────

    void gen_decl(const Decl *d);
    void gen_fn(const Decl *d);
    void gen_struct_type(const Decl *d);
    void gen_enum_type(const Decl *d);
    void gen_global_const(const Decl *d);

    // ── Statement codegen ─────────────────────────────────────────────────────

    void gen_stmts(const Stmt **stmts, size_t count);
    void gen_stmt(const Stmt *s);
    void gen_let(const Stmt *s);
    void gen_return(const Stmt *s);
    void gen_defer_setup();     // emit landing pad for defers
    void gen_for(const Stmt *s);
    void gen_while(const Stmt *s);

    // ── Expression codegen ────────────────────────────────────────────────────

    IRValue gen_expr(const Expr *e);
    IRValue gen_int_lit(const Expr *e);
    IRValue gen_float_lit(const Expr *e);
    IRValue gen_bool_lit(const Expr *e);
    IRValue gen_str_lit(const Expr *e);
    IRValue gen_ident(const Expr *e);
    IRValue gen_binary(const Expr *e);
    IRValue gen_unary(const Expr *e);
    IRValue gen_call(const Expr *e);
    IRValue gen_if(const Expr *e);
    IRValue gen_match(const Expr *e);
    IRValue gen_block(const Expr *e);
    IRValue gen_struct_lit(const Expr *e);
    IRValue gen_field(const Expr *e);
    IRValue gen_index(const Expr *e);
    IRValue gen_builtin(const Expr *e);

    // ── Type mapping ──────────────────────────────────────────────────────────

    void *llvm_type(const Type *t);   // returns llvm::Type*
    void *llvm_prim(PrimKind p);      // returns llvm::Type*

    // ── Ownership / borrow checks (integrated into codegen) ──────────────────

    void check_move(const Expr *e);
    void check_borrow(const Expr *e, bool is_mut);
    void invalidate_moved(const char *name);

private:
    std::unique_ptr<LLVMState>  state_;
    DiagSink                   *diags_;
    CodegenOptions              opts_;

    // Variable table: name → alloca ptr + type
    struct VarEntry {
        void *alloca;      // llvm::AllocaInst*
        const Type *ty;
        bool  moved;
        bool  is_mut;
    };
    std::vector<std::unordered_map<std::string, VarEntry>> scope_stack_;

    // Deferred statement stack (for defer)
    struct DeferEntry { const Stmt *stmt; };
    std::vector<std::vector<DeferEntry>> defer_stack_;

    // ── Scope helpers ──────────────────────────────────────────────────────────
    void push_scope();
    void pop_scope();
    void define(const std::string &name, void *alloca, const Type *ty, bool is_mut);
    std::optional<VarEntry> lookup(const std::string &name) const;

    // ── IR emission helpers ───────────────────────────────────────────────────
    IRValue load_if_lvalue(IRValue v);
    void   *current_fn();   // returns llvm::Function*
    void   *current_bb();   // returns llvm::BasicBlock*
    void   *make_bb(const char *name);
    void    set_bb(void *bb);

    // ── Error helpers ─────────────────────────────────────────────────────────
    IRValue error_val(const SrcSpan &span, const char *msg);
};

// ─── Ownership checker (standalone pass before codegen) ──────────────────────

struct OwnershipError {
    SrcSpan     span;
    std::string message;
    std::string note;
};

std::vector<OwnershipError> check_ownership(const AstFile *ast);

} // namespace codegen
} // namespace rsharp
