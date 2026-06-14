// compiler/codegen/llvm_backend.cpp — R# LLVM Backend  C++20
// Complete implementation: AST -> LLVM IR -> native object file
// Requires LLVM 17+

#ifdef RSHARP_WITH_LLVM

#include "llvm_backend.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils.h>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

extern "C" {
#include "../parser/ast.h"
#include "../sema/sema.h"
#include "../error/diagnostic.h"
#include "../../runtime/memory/arena.h"
}

namespace rsharp {
namespace codegen {

using namespace llvm;

/* ── Internal state ──────────────────────────────────────────────── */
struct LLVMState {
    LLVMContext                       ctx;
    std::unique_ptr<Module>           mod;
    std::unique_ptr<IRBuilder<>>      builder;
    TargetMachine                    *target_machine = nullptr;

    // Variable table: maps variable name -> alloca instruction
    struct VarEntry {
        AllocaInst  *alloca;
        llvm::Type  *type;
        bool         is_mut;
    };
    std::vector<std::unordered_map<std::string, VarEntry>> scopes;
    std::unordered_map<std::string, Function*>             fns;

    // Current function context
    Function     *current_fn   = nullptr;
    BasicBlock   *current_bb   = nullptr;
    BasicBlock   *break_target = nullptr;
    BasicBlock   *cont_target  = nullptr;

    // Deferred blocks (for defer statement)
    std::vector<std::vector<const Stmt*>> defer_stack;
};

/* ── Type mapping ─────────────────────────────────────────────────── */
static llvm::Type *llvm_prim(LLVMContext &ctx, PrimKind p) {
    switch (p) {
        case PRIM_I8:  case PRIM_U8:   return Type::getInt8Ty(ctx);
        case PRIM_I16: case PRIM_U16:  return Type::getInt16Ty(ctx);
        case PRIM_I32: case PRIM_U32:  return Type::getInt32Ty(ctx);
        case PRIM_I64: case PRIM_U64:  return Type::getInt64Ty(ctx);
        case PRIM_I128:case PRIM_U128: return Type::getInt128Ty(ctx);
        case PRIM_F32: return Type::getFloatTy(ctx);
        case PRIM_F64: return Type::getDoubleTy(ctx);
        case PRIM_BOOL:return Type::getInt1Ty(ctx);
        case PRIM_CHAR:return Type::getInt32Ty(ctx); // Unicode scalar
        case PRIM_VOID:return Type::getVoidTy(ctx);
        default:       return Type::getInt32Ty(ctx);
    }
}

static llvm::Type *llvm_type(LLVMContext &ctx, const Type *t) {
    if (!t) return Type::getVoidTy(ctx);
    switch (t->kind) {
        case TY_PRIM:    return llvm_prim(ctx, t->prim);
        case TY_PTR:     return PointerType::get(llvm_type(ctx, t->ptr.inner), 0);
        case TY_SLICE:   {
            // Fat pointer: { ptr, len }
            std::vector<llvm::Type*> fields = {
                PointerType::get(llvm_type(ctx, t->slice.elem), 0),
                Type::getInt64Ty(ctx)
            };
            return StructType::get(ctx, fields);
        }
        case TY_OPTIONAL:{
            // { value, has_value: i1 }
            std::vector<llvm::Type*> fields = {
                llvm_type(ctx, t->optional.inner),
                Type::getInt1Ty(ctx)
            };
            return StructType::get(ctx, fields);
        }
        case TY_INFER:   return Type::getInt32Ty(ctx);
        default:         return Type::getInt8PtrTy(ctx);
    }
}

static llvm::Type *llvm_resolved(LLVMContext &ctx, const ResolvedType *t) {
    if (!t) return Type::getVoidTy(ctx);
    switch (t->kind) {
        case RTYPE_VOID:  return Type::getVoidTy(ctx);
        case RTYPE_NEVER: return Type::getVoidTy(ctx);
        case RTYPE_INT: {
            switch (t->int_ty.width) {
                case 8:  return Type::getInt8Ty(ctx);
                case 16: return Type::getInt16Ty(ctx);
                case 32: return Type::getInt32Ty(ctx);
                case 64: return Type::getInt64Ty(ctx);
                case 128:return Type::getInt128Ty(ctx);
                default: return Type::getInt32Ty(ctx);
            }
        }
        case RTYPE_FLOAT:
            return t->float_ty.width == 32 ? Type::getFloatTy(ctx) : Type::getDoubleTy(ctx);
        case RTYPE_BOOL:  return Type::getInt1Ty(ctx);
        case RTYPE_CHAR:  return Type::getInt32Ty(ctx);
        case RTYPE_STR: {
            // &str = { ptr, len }
            return StructType::get(ctx,
                { PointerType::get(Type::getInt8Ty(ctx), 0), Type::getInt64Ty(ctx) });
        }
        case RTYPE_PTR:
            return PointerType::get(llvm_resolved(ctx, t->ptr_ty.inner), 0);
        default:
            return Type::getInt32Ty(ctx);
    }
}

/* ── Backend class ───────────────────────────────────────────────── */
class Backend {
    LLVMState   st_;
    DiagSink   *diags_;
    CodegenOptions opts_;
    const char *filepath_ = "";

public:
    Backend(DiagSink *diags, CodegenOptions opts)
        : diags_(diags), opts_(opts)
    {
        InitializeAllTargetInfos();
        InitializeAllTargets();
        InitializeAllTargetMCs();
        InitializeAllAsmPrinters();
        InitializeAllAsmParsers();
    }

    bool init_target(const std::string &triple_str) {
        std::string triple = triple_str.empty()
            ? sys::getDefaultTargetTriple()
            : triple_str;
        std::string err;
        const Target *target = TargetRegistry::lookupTarget(triple, err);
        if (!target) {
            diag_error(diags_, filepath_, nullptr, {{0,0,0},{0,0,0}},
                       err.c_str(), "ensure LLVM target is compiled in", nullptr);
            return false;
        }
        TargetOptions opt;
        auto rm = opts_.opt > OptLevel::Debug
            ? std::optional<Reloc::Model>(Reloc::PIC_) : std::nullopt;
        st_.target_machine = target->createTargetMachine(
            triple, opts_.cpu.empty() ? "generic" : opts_.cpu,
            opts_.features, opt, rm);
        return st_.target_machine != nullptr;
    }

    bool compile_file(const AstFile *ast, const std::string &out_path) {
        filepath_ = ast->path;
        st_.mod = std::make_unique<Module>(
            ast->path ? ast->path : "rsharp_module", st_.ctx);
        st_.builder = std::make_unique<IRBuilder<>>(st_.ctx);

        std::string triple = opts_.triple.empty()
            ? sys::getDefaultTargetTriple() : opts_.triple;
        if (!init_target(triple)) return false;
        st_.mod->setTargetTriple(triple);
        st_.mod->setDataLayout(st_.target_machine->createDataLayout());

        // Declare external builtins
        declare_builtins();

        // First pass: forward-declare all functions
        for (size_t i = 0; i < ast->declc; i++)
            if (ast->decls[i]->kind == DECL_FN)
                declare_fn(ast->decls[i]);

        // Second pass: generate function bodies
        for (size_t i = 0; i < ast->declc; i++) {
            Decl *d = ast->decls[i];
            if (d->kind == DECL_FN && d->fn.body)
                gen_fn(d);
        }

        // Verify
        std::string verr;
        raw_string_ostream verr_stream(verr);
        if (verifyModule(*st_.mod, &verr_stream)) {
            diag_error(diags_, filepath_, nullptr, {{0,0,0},{0,0,0}},
                       "LLVM IR verification failed", verr.c_str(), nullptr);
            return false;
        }

        // Optimise
        optimise();

        // Emit IR if requested
        if (opts_.emit_ir) {
            std::string ir_path = out_path + ".ll";
            std::error_code ec;
            raw_fd_ostream ir_out(ir_path, ec, sys::fs::OF_None);
            if (!ec) st_.mod->print(ir_out, nullptr);
        }

        // Emit object file
        std::error_code ec;
        raw_fd_ostream dest(out_path, ec, sys::fs::OF_None);
        if (ec) {
            diag_error(diags_, filepath_, nullptr, {{0,0,0},{0,0,0}},
                       ec.message().c_str(), nullptr, nullptr);
            return false;
        }
        legacy::PassManager pm;
        if (st_.target_machine->addPassesToEmitFile(
                pm, dest, nullptr, CodeGenFileType::ObjectFile)) {
            diag_error(diags_, filepath_, nullptr, {{0,0,0},{0,0,0}},
                       "target does not support object emission", nullptr, nullptr);
            return false;
        }
        pm.run(*st_.mod);
        dest.flush();
        return true;
    }

    std::string emit_ir_string(const AstFile *ast) {
        filepath_ = ast->path;
        st_.mod = std::make_unique<Module>(
            ast->path ? ast->path : "rsharp_module", st_.ctx);
        st_.builder = std::make_unique<IRBuilder<>>(st_.ctx);
        declare_builtins();
        for (size_t i = 0; i < ast->declc; i++)
            if (ast->decls[i]->kind == DECL_FN)
                declare_fn(ast->decls[i]);
        for (size_t i = 0; i < ast->declc; i++)
            if (ast->decls[i]->kind == DECL_FN && ast->decls[i]->fn.body)
                gen_fn(ast->decls[i]);
        std::string out;
        raw_string_ostream s(out);
        st_.mod->print(s, nullptr);
        return out;
    }

private:
    void declare_builtins() {
        // @print / rsharp_print: void(i8*, i64)
        FunctionType *print_ty = FunctionType::get(
            Type::getVoidTy(st_.ctx),
            { PointerType::get(Type::getInt8Ty(st_.ctx), 0),
              Type::getInt64Ty(st_.ctx) }, true);
        st_.mod->getOrInsertFunction("rsharp_print", print_ty);

        // @assert: void(i1, i8*)
        FunctionType *assert_ty = FunctionType::get(
            Type::getVoidTy(st_.ctx),
            { Type::getInt1Ty(st_.ctx),
              PointerType::get(Type::getInt8Ty(st_.ctx), 0) }, false);
        st_.mod->getOrInsertFunction("rsharp_assert", assert_ty);

        // @panic: void(i8*) noreturn
        FunctionType *panic_ty = FunctionType::get(
            Type::getVoidTy(st_.ctx),
            { PointerType::get(Type::getInt8Ty(st_.ctx), 0) }, false);
        Function *panic_fn = Function::Create(
            panic_ty, Function::ExternalLinkage, "rsharp_panic", st_.mod.get());
        panic_fn->addFnAttr(Attribute::NoReturn);

        // sqrt, sin, cos, etc. (LLVM intrinsics)
        auto intr = [&](const char *name) {
            FunctionType *ft = FunctionType::get(
                Type::getDoubleTy(st_.ctx),
                { Type::getDoubleTy(st_.ctx) }, false);
            st_.mod->getOrInsertFunction(name, ft);
        };
        intr("llvm.sqrt.f64"); intr("llvm.sin.f64"); intr("llvm.cos.f64");
        intr("llvm.floor.f64"); intr("llvm.ceil.f64"); intr("llvm.fabs.f64");
    }

    llvm::Type *fn_ret_type(const Decl *d) {
        if (!d->fn.ret_ty) return Type::getVoidTy(st_.ctx);
        return llvm_type(st_.ctx, d->fn.ret_ty);
    }

    Function *declare_fn(const Decl *d) {
        std::vector<llvm::Type*> param_types;
        for (size_t i = 0; i < d->fn.paramc; i++) {
            Param &p = d->fn.params[i];
            param_types.push_back(p.ty ? llvm_type(st_.ctx, p.ty)
                                       : Type::getInt32Ty(st_.ctx));
        }
        FunctionType *ft = FunctionType::get(fn_ret_type(d), param_types, false);
        std::string name(d->fn.name.ptr, d->fn.name.len);
        Function *fn = Function::Create(
            ft,
            d->is_pub ? Function::ExternalLinkage : Function::InternalLinkage,
            name, st_.mod.get());
        // Name parameters
        size_t i = 0;
        for (auto &arg : fn->args()) {
            if (i < d->fn.paramc) {
                std::string pname(d->fn.params[i].name.ptr, d->fn.params[i].name.len);
                arg.setName(pname);
            }
            i++;
        }
        st_.fns[name] = fn;
        return fn;
    }

    void gen_fn(const Decl *d) {
        std::string name(d->fn.name.ptr, d->fn.name.len);
        Function *fn = st_.fns.count(name) ? st_.fns[name] : declare_fn(d);
        st_.current_fn = fn;
        BasicBlock *entry = BasicBlock::Create(st_.ctx, "entry", fn);
        st_.builder->SetInsertPoint(entry);
        st_.current_bb = entry;

        st_.scopes.push_back({});
        st_.defer_stack.push_back({});

        // Allocate parameters on stack
        size_t i = 0;
        for (auto &arg : fn->args()) {
            AllocaInst *alloca = st_.builder->CreateAlloca(
                arg.getType(), nullptr, arg.getName());
            st_.builder->CreateStore(&arg, alloca);
            std::string pname = std::string(arg.getName());
            st_.scopes.back()[pname] = {alloca, arg.getType(), false};
            i++;
        }

        // Generate body
        for (size_t j = 0; j < d->fn.bodyc; j++)
            gen_stmt(d->fn.body[j]);

        // Run deferred blocks
        run_defers();

        // Ensure function ends properly
        if (!st_.builder->GetInsertBlock()->getTerminator()) {
            if (fn_ret_type(d)->isVoidTy()) {
                st_.builder->CreateRetVoid();
            } else {
                st_.builder->CreateRet(
                    Constant::getNullValue(fn_ret_type(d)));
            }
        }

        st_.scopes.pop_back();
        st_.defer_stack.pop_back();
        st_.current_fn = nullptr;
    }

    // Deferred statements are stored as AST Stmt* and emitted inline at scope exit
    void run_defers() {
        if (st_.defer_stack.empty()) return;
        auto &defers = st_.defer_stack.back();
        // Execute in LIFO order (last defer runs first)
        for (int i = (int)defers.size() - 1; i >= 0; i--) {
            gen_stmt(defers[i]);
        }
        defers.clear();
    }

    // ── Alloca helper (always at function entry for consistent stack layout)
    AllocaInst *create_entry_alloca(llvm::Type *ty, const std::string &name) {
        IRBuilder<> tmp(&st_.current_fn->getEntryBlock(),
                        st_.current_fn->getEntryBlock().begin());
        return tmp.CreateAlloca(ty, nullptr, name);
    }

    void define_var(const std::string &name, AllocaInst *alloca, llvm::Type *ty, bool mut) {
        if (!st_.scopes.empty())
            st_.scopes.back()[name] = {alloca, ty, mut};
    }

    AllocaInst *lookup_var(const std::string &name) {
        for (int i = (int)st_.scopes.size()-1; i >= 0; i--) {
            auto it = st_.scopes[i].find(name);
            if (it != st_.scopes[i].end()) return it->second.alloca;
        }
        return nullptr;
    }

    // ── Statements ───────────────────────────────────────────────────
    void gen_stmt(const Stmt *s) {
        if (!s) return;
        switch (s->kind) {
            case STMT_LET: {
                llvm::Type *ty = s->let.ty ? llvm_type(st_.ctx, s->let.ty)
                                           : Type::getInt32Ty(st_.ctx);
                if (s->let.init && s->let.init->type)
                    ty = llvm_resolved(st_.ctx, s->let.init->type);
                std::string vname(s->let.name.ptr, s->let.name.len);
                AllocaInst *alloca = create_entry_alloca(ty, vname);
                if (s->let.init) {
                    Value *init_val = gen_expr(s->let.init);
                    if (init_val && init_val->getType() != ty)
                        init_val = coerce(init_val, ty);
                    if (init_val) st_.builder->CreateStore(init_val, alloca);
                }
                define_var(vname, alloca, ty, s->let.is_mut);
                break;
            }
            case STMT_RETURN: {
                run_defers();
                if (s->ret.value) {
                    Value *v = gen_expr(s->ret.value);
                    llvm::Type *ret_ty = st_.current_fn->getReturnType();
                    if (v && v->getType() != ret_ty) v = coerce(v, ret_ty);
                    if (v) st_.builder->CreateRet(v);
                    else   st_.builder->CreateRetVoid();
                } else {
                    st_.builder->CreateRetVoid();
                }
                // Start unreachable block for code after return
                BasicBlock *dead = BasicBlock::Create(st_.ctx,"after_ret",st_.current_fn);
                st_.builder->SetInsertPoint(dead);
                break;
            }
            case STMT_EXPR: {
                gen_expr(s->expr_stmt);
                break;
            }
            case STMT_FOR: {
                // for bind in start..end { body }
                BasicBlock *cond_bb = BasicBlock::Create(st_.ctx,"for_cond",st_.current_fn);
                BasicBlock *body_bb = BasicBlock::Create(st_.ctx,"for_body",st_.current_fn);
                BasicBlock *incr_bb = BasicBlock::Create(st_.ctx,"for_incr",st_.current_fn);
                BasicBlock *end_bb  = BasicBlock::Create(st_.ctx,"for_end", st_.current_fn);

                // Allocate loop variable
                std::string bind(s->for_s.bind.ptr, s->for_s.bind.len);
                AllocaInst *loop_var = create_entry_alloca(Type::getInt64Ty(st_.ctx), bind);
                define_var(bind, loop_var, Type::getInt64Ty(st_.ctx), true);

                // Get range start/end from iter expr (simplified: expect range)
                Value *start_val = ConstantInt::get(Type::getInt64Ty(st_.ctx), 0);
                Value *end_val   = ConstantInt::get(Type::getInt64Ty(st_.ctx), 0);
                if (s->for_s.iter && s->for_s.iter->kind == EXPR_RANGE) {
                    start_val = gen_expr(s->for_s.iter->range.start);
                    end_val   = gen_expr(s->for_s.iter->range.end);
                    if (start_val) start_val = coerce(start_val, Type::getInt64Ty(st_.ctx));
                    if (end_val)   end_val   = coerce(end_val,   Type::getInt64Ty(st_.ctx));
                }
                if (!start_val) start_val = ConstantInt::get(Type::getInt64Ty(st_.ctx),0);
                if (!end_val)   end_val   = ConstantInt::get(Type::getInt64Ty(st_.ctx),0);
                st_.builder->CreateStore(start_val, loop_var);
                st_.builder->CreateBr(cond_bb);

                // Condition
                st_.builder->SetInsertPoint(cond_bb);
                Value *cur = st_.builder->CreateLoad(Type::getInt64Ty(st_.ctx), loop_var, "i");
                Value *cond = s->for_s.iter && s->for_s.iter->kind==EXPR_RANGE && s->for_s.iter->range.inclusive
                    ? st_.builder->CreateICmpSLE(cur, end_val, "for_cond")
                    : st_.builder->CreateICmpSLT(cur, end_val, "for_cond");
                st_.builder->CreateCondBr(cond, body_bb, end_bb);

                // Body
                st_.builder->SetInsertPoint(body_bb);
                st_.scopes.push_back({});
                BasicBlock *old_break = st_.break_target;
                BasicBlock *old_cont  = st_.cont_target;
                st_.break_target = end_bb;
                st_.cont_target  = incr_bb;
                for (size_t i = 0; i < s->for_s.c; i++) gen_stmt(s->for_s.body[i]);
                st_.break_target = old_break;
                st_.cont_target  = old_cont;
                st_.scopes.pop_back();
                if (!st_.builder->GetInsertBlock()->getTerminator())
                    st_.builder->CreateBr(incr_bb);

                // Increment
                st_.builder->SetInsertPoint(incr_bb);
                Value *next = st_.builder->CreateAdd(
                    st_.builder->CreateLoad(Type::getInt64Ty(st_.ctx), loop_var),
                    ConstantInt::get(Type::getInt64Ty(st_.ctx), 1), "i_next");
                st_.builder->CreateStore(next, loop_var);
                st_.builder->CreateBr(cond_bb);

                st_.builder->SetInsertPoint(end_bb);
                break;
            }
            case STMT_WHILE: {
                BasicBlock *cond_bb = BasicBlock::Create(st_.ctx,"while_cond",st_.current_fn);
                BasicBlock *body_bb = BasicBlock::Create(st_.ctx,"while_body",st_.current_fn);
                BasicBlock *end_bb  = BasicBlock::Create(st_.ctx,"while_end", st_.current_fn);
                st_.builder->CreateBr(cond_bb);
                st_.builder->SetInsertPoint(cond_bb);
                Value *cond = gen_expr(s->while_s.cond);
                if (cond && cond->getType() != Type::getInt1Ty(st_.ctx))
                    cond = st_.builder->CreateICmpNE(cond,
                        Constant::getNullValue(cond->getType()), "while_cond");
                if (!cond) cond = ConstantInt::getFalse(st_.ctx);
                st_.builder->CreateCondBr(cond, body_bb, end_bb);
                st_.builder->SetInsertPoint(body_bb);
                st_.scopes.push_back({});
                BasicBlock *ob = st_.break_target, *oc = st_.cont_target;
                st_.break_target = end_bb; st_.cont_target = cond_bb;
                for (size_t i = 0; i < s->while_s.c; i++) gen_stmt(s->while_s.body[i]);
                st_.break_target = ob; st_.cont_target = oc;
                st_.scopes.pop_back();
                if (!st_.builder->GetInsertBlock()->getTerminator())
                    st_.builder->CreateBr(cond_bb);
                st_.builder->SetInsertPoint(end_bb);
                break;
            }
            case STMT_BREAK:
                if (st_.break_target) {
                    run_defers();
                    st_.builder->CreateBr(st_.break_target);
                    BasicBlock *dead=BasicBlock::Create(st_.ctx,"after_break",st_.current_fn);
                    st_.builder->SetInsertPoint(dead);
                }
                break;
            case STMT_CONTINUE:
                if (st_.cont_target) {
                    st_.builder->CreateBr(st_.cont_target);
                    BasicBlock *dead=BasicBlock::Create(st_.ctx,"after_cont",st_.current_fn);
                    st_.builder->SetInsertPoint(dead);
                }
                break;
            case STMT_DEFER: {
                // Store the deferred statement for later execution at scope exit
                if (!st_.defer_stack.empty())
                    st_.defer_stack.back().push_back((const Stmt*)s->defer_s.inner);
                break;
            }
            case STMT_UNSAFE: {
                st_.scopes.push_back({});
                for (size_t i = 0; i < s->unsafe_s.count; i++) gen_stmt(s->unsafe_s.body[i]);
                st_.scopes.pop_back();
                break;
            }
            default: break;
        }
    }

    // ── Expressions ──────────────────────────────────────────────────
    Value *gen_expr(const Expr *e) {
        if (!e) return nullptr;
        switch (e->kind) {
            case EXPR_INT_LIT:
                return ConstantInt::get(Type::getInt64Ty(st_.ctx), (uint64_t)e->int_val, true);
            case EXPR_FLOAT_LIT:
                return ConstantFP::get(Type::getDoubleTy(st_.ctx), e->flt_val);
            case EXPR_BOOL_LIT:
                return e->bool_val ? ConstantInt::getTrue(st_.ctx) : ConstantInt::getFalse(st_.ctx);
            case EXPR_STR_LIT: {
                // Build { ptr, len } fat pointer
                Constant *str_data = ConstantDataArray::getString(
                    st_.ctx, StringRef(e->str_val.ptr, e->str_val.len), false);
                GlobalVariable *gv = new GlobalVariable(
                    *st_.mod, str_data->getType(), true,
                    GlobalValue::PrivateLinkage, str_data, ".str");
                gv->setAlignment(Align(1));
                Constant *zero = ConstantInt::get(Type::getInt32Ty(st_.ctx), 0);
                Constant *ptr = ConstantExpr::getGetElementPtr(
                    str_data->getType(), gv, ArrayRef<Constant*>{zero, zero});
                Constant *len = ConstantInt::get(Type::getInt64Ty(st_.ctx), e->str_val.len);
                llvm::Type *str_ty = StructType::get(st_.ctx,
                    { PointerType::get(Type::getInt8Ty(st_.ctx),0), Type::getInt64Ty(st_.ctx) });
                return ConstantStruct::get(cast<StructType>(str_ty), {ptr, len});
            }
            case EXPR_IDENT: {
                std::string name(e->ident.ptr, e->ident.len);
                AllocaInst *alloca = lookup_var(name);
                if (!alloca) {
                    // Could be a function reference
                    auto it = st_.fns.find(name);
                    if (it != st_.fns.end()) return it->second;
                    return nullptr;
                }
                return st_.builder->CreateLoad(alloca->getAllocatedType(), alloca, name);
            }
            case EXPR_UNARY: {
                Value *v = gen_expr(e->unary.operand);
                if (!v) return nullptr;
                switch (e->unary.op) {
                    case UNOP_NEG:
                        if (v->getType()->isFloatingPointTy())
                            return st_.builder->CreateFNeg(v);
                        return st_.builder->CreateNeg(v);
                    case UNOP_NOT:
                        return st_.builder->CreateNot(v);
                    case UNOP_BITNOT:
                        return st_.builder->CreateNot(v);
                    default:
                        return v;
                }
            }
            case EXPR_BINARY: {
                // Handle assignment separately
                if (e->binary.op == BINOP_ASSIGN) {
                    Value *rhs = gen_expr(e->binary.rhs);
                    if (e->binary.lhs->kind == EXPR_IDENT) {
                        std::string name(e->binary.lhs->ident.ptr, e->binary.lhs->ident.len);
                        AllocaInst *alloca = lookup_var(name);
                        if (alloca && rhs) {
                            if (rhs->getType() != alloca->getAllocatedType())
                                rhs = coerce(rhs, alloca->getAllocatedType());
                            st_.builder->CreateStore(rhs, alloca);
                        }
                    }
                    return rhs;
                }
                Value *lhs = gen_expr(e->binary.lhs);
                Value *rhs = gen_expr(e->binary.rhs);
                if (!lhs || !rhs) return lhs ? lhs : rhs;
                // Coerce types
                if (lhs->getType() != rhs->getType()) {
                    rhs = coerce(rhs, lhs->getType());
                    if (!rhs) return lhs;
                }
                bool fp = lhs->getType()->isFloatingPointTy();
                bool is_signed = true; // default to signed
                switch (e->binary.op) {
                    case BINOP_ADD: return fp ? st_.builder->CreateFAdd(lhs,rhs)
                                              : st_.builder->CreateAdd(lhs,rhs);
                    case BINOP_SUB: return fp ? st_.builder->CreateFSub(lhs,rhs)
                                              : st_.builder->CreateSub(lhs,rhs);
                    case BINOP_MUL: return fp ? st_.builder->CreateFMul(lhs,rhs)
                                              : st_.builder->CreateMul(lhs,rhs);
                    case BINOP_DIV: return fp ? st_.builder->CreateFDiv(lhs,rhs)
                                              : (is_signed ? st_.builder->CreateSDiv(lhs,rhs)
                                                           : st_.builder->CreateUDiv(lhs,rhs));
                    case BINOP_MOD: return fp ? st_.builder->CreateFRem(lhs,rhs)
                                              : st_.builder->CreateSRem(lhs,rhs);
                    case BINOP_AND: return st_.builder->CreateAnd(lhs,rhs);
                    case BINOP_OR:  return st_.builder->CreateOr(lhs,rhs);
                    case BINOP_BITAND: return st_.builder->CreateAnd(lhs,rhs);
                    case BINOP_BITOR:  return st_.builder->CreateOr(lhs,rhs);
                    case BINOP_BITXOR: return st_.builder->CreateXor(lhs,rhs);
                    case BINOP_SHL: return st_.builder->CreateShl(lhs,rhs);
                    case BINOP_SHR: return st_.builder->CreateAShr(lhs,rhs);
                    case BINOP_EQ:  return fp ? st_.builder->CreateFCmpOEQ(lhs,rhs)
                                              : st_.builder->CreateICmpEQ(lhs,rhs);
                    case BINOP_NEQ: return fp ? st_.builder->CreateFCmpONE(lhs,rhs)
                                              : st_.builder->CreateICmpNE(lhs,rhs);
                    case BINOP_LT:  return fp ? st_.builder->CreateFCmpOLT(lhs,rhs)
                                              : st_.builder->CreateICmpSLT(lhs,rhs);
                    case BINOP_GT:  return fp ? st_.builder->CreateFCmpOGT(lhs,rhs)
                                              : st_.builder->CreateICmpSGT(lhs,rhs);
                    case BINOP_LE:  return fp ? st_.builder->CreateFCmpOLE(lhs,rhs)
                                              : st_.builder->CreateICmpSLE(lhs,rhs);
                    case BINOP_GE:  return fp ? st_.builder->CreateFCmpOGE(lhs,rhs)
                                              : st_.builder->CreateICmpSGE(lhs,rhs);
                    case BINOP_NULLCOAL: {
                        // a ?? b: if a is non-null return a else b
                        // Simplified: just return lhs
                        return lhs;
                    }
                    default: return lhs;
                }
            }
            case EXPR_CALL: {
                // Get callee
                Value *callee_val = nullptr;
                Function *callee_fn = nullptr;
                if (e->call.callee->kind == EXPR_IDENT) {
                    std::string fname(e->call.callee->ident.ptr, e->call.callee->ident.len);
                    auto it = st_.fns.find(fname);
                    if (it != st_.fns.end()) {
                        callee_fn = it->second;
                        callee_val = it->second;
                    }
                }
                if (!callee_fn) {
                    callee_val = gen_expr(e->call.callee);
                    if (!callee_val) return nullptr;
                }
                std::vector<Value*> args;
                size_t fn_params = callee_fn ? callee_fn->arg_size() : e->call.argc;
                for (size_t i = 0; i < e->call.argc; i++) {
                    Value *av = gen_expr(e->call.args[i]);
                    if (!av) continue;
                    if (callee_fn && i < fn_params) {
                        llvm::Type *expected = callee_fn->getArg(i)->getType();
                        if (av->getType() != expected) av = coerce(av, expected);
                    }
                    args.push_back(av);
                }
                if (callee_fn) {
                    return st_.builder->CreateCall(callee_fn->getFunctionType(), callee_fn, args);
                }
                return nullptr;
            }
            case EXPR_BUILTIN: {
                std::string bname(e->builtin.name.ptr, e->builtin.name.len);
                return gen_builtin(bname, e);
            }
            case EXPR_IF: {
                Value *cond = gen_expr(e->if_expr.cond);
                if (!cond) return nullptr;
                if (cond->getType() != Type::getInt1Ty(st_.ctx))
                    cond = st_.builder->CreateICmpNE(cond,
                        Constant::getNullValue(cond->getType()), "if_cond");

                Function *fn = st_.current_fn;
                BasicBlock *then_bb = BasicBlock::Create(st_.ctx,"then",fn);
                BasicBlock *else_bb = BasicBlock::Create(st_.ctx,"else",fn);
                BasicBlock *merge_bb= BasicBlock::Create(st_.ctx,"merge",fn);
                st_.builder->CreateCondBr(cond, then_bb, else_bb);

                st_.builder->SetInsertPoint(then_bb);
                Value *then_v = gen_expr(e->if_expr.then_expr);
                if (!st_.builder->GetInsertBlock()->getTerminator())
                    st_.builder->CreateBr(merge_bb);
                BasicBlock *then_end = st_.builder->GetInsertBlock();

                st_.builder->SetInsertPoint(else_bb);
                Value *else_v = e->if_expr.else_expr
                    ? gen_expr(e->if_expr.else_expr) : nullptr;
                if (!st_.builder->GetInsertBlock()->getTerminator())
                    st_.builder->CreateBr(merge_bb);
                BasicBlock *else_end = st_.builder->GetInsertBlock();

                st_.builder->SetInsertPoint(merge_bb);
                if (then_v && else_v && then_v->getType() == else_v->getType()
                    && !then_v->getType()->isVoidTy()) {
                    PHINode *phi = st_.builder->CreatePHI(then_v->getType(), 2, "if_val");
                    phi->addIncoming(then_v, then_end);
                    phi->addIncoming(else_v, else_end);
                    return phi;
                }
                return nullptr;
            }
            case EXPR_BLOCK: {
                st_.scopes.push_back({});
                Value *last = nullptr;
                for (size_t i = 0; i < e->block.stmtc; i++) gen_stmt(e->block.stmts[i]);
                if (e->block.tail) last = gen_expr(e->block.tail);
                st_.scopes.pop_back();
                return last;
            }
            default:
                return nullptr;
        }
    }

    Value *gen_builtin(const std::string &name, const Expr *e) {
        if (name == "print" || name == "println") {
            Function *print_fn = st_.mod->getFunction("rsharp_print");
            if (!print_fn || e->builtin.argc == 0) return nullptr;
            Value *msg = gen_expr(e->builtin.args[0]);
            if (!msg) return nullptr;
            // Extract ptr from fat string struct
            Value *ptr = st_.builder->CreateExtractValue(msg, {0}, "str_ptr");
            Value *len = st_.builder->CreateExtractValue(msg, {1}, "str_len");
            std::vector<Value*> call_args = {ptr, len};
            // Append extra args
            for (size_t i = 1; i < e->builtin.argc; i++) {
                Value *arg = gen_expr(e->builtin.args[i]);
                if (arg) call_args.push_back(arg);
            }
            return st_.builder->CreateCall(
                print_fn->getFunctionType(), print_fn, call_args);
        }
        if (name == "assert") {
            Function *fn = st_.mod->getFunction("rsharp_assert");
            if (!fn || e->builtin.argc < 1) return nullptr;
            Value *cond = gen_expr(e->builtin.args[0]);
            if (!cond) return nullptr;
            Value *msg_ptr = ConstantPointerNull::get(
                PointerType::get(Type::getInt8Ty(st_.ctx),0));
            return st_.builder->CreateCall(fn->getFunctionType(), fn, {cond, msg_ptr});
        }
        if (name == "panic") {
            Function *fn = st_.mod->getFunction("rsharp_panic");
            if (!fn || e->builtin.argc < 1) return nullptr;
            Value *msg = gen_expr(e->builtin.args[0]);
            Value *ptr = st_.builder->CreateExtractValue(msg, {0});
            st_.builder->CreateCall(fn->getFunctionType(), fn, {ptr});
            st_.builder->CreateUnreachable();
            BasicBlock *dead = BasicBlock::Create(st_.ctx,"after_panic",st_.current_fn);
            st_.builder->SetInsertPoint(dead);
            return nullptr;
        }
        // Math builtins via LLVM intrinsics
        auto llvm_intr = [&](const char *iname) -> Value* {
            Function *fn = st_.mod->getFunction(iname);
            if (!fn || e->builtin.argc < 1) return nullptr;
            Value *arg = gen_expr(e->builtin.args[0]);
            if (!arg) return nullptr;
            arg = coerce(arg, Type::getDoubleTy(st_.ctx));
            return st_.builder->CreateCall(fn->getFunctionType(), fn, {arg});
        };
        if (name=="sqrt")  return llvm_intr("llvm.sqrt.f64");
        if (name=="sin")   return llvm_intr("llvm.sin.f64");
        if (name=="cos")   return llvm_intr("llvm.cos.f64");
        if (name=="floor") return llvm_intr("llvm.floor.f64");
        if (name=="ceil")  return llvm_intr("llvm.ceil.f64");
        if (name=="abs")   return llvm_intr("llvm.fabs.f64");
        if (name=="sizeof") {
            // Return size of type as i64
            if (e->builtin.argc < 1) return ConstantInt::get(Type::getInt64Ty(st_.ctx),0);
            return ConstantInt::get(Type::getInt64Ty(st_.ctx), 8); // simplified
        }
        return nullptr;
    }

    // ── Type coercion ─────────────────────────────────────────────────
    Value *coerce(Value *v, llvm::Type *target) {
        if (!v || !target) return v;
        llvm::Type *src = v->getType();
        if (src == target) return v;
        if (src->isIntegerTy() && target->isIntegerTy()) {
            if (src->getIntegerBitWidth() < target->getIntegerBitWidth())
                return st_.builder->CreateSExt(v, target, "sext");
            else
                return st_.builder->CreateTrunc(v, target, "trunc");
        }
        if (src->isIntegerTy() && target->isFloatingPointTy())
            return st_.builder->CreateSIToFP(v, target, "sitofp");
        if (src->isFloatingPointTy() && target->isIntegerTy())
            return st_.builder->CreateFPToSI(v, target, "fptosi");
        if (src->isFloatingPointTy() && target->isFloatingPointTy())
            return st_.builder->CreateFPCast(v, target, "fpcast");
        if (src->isPointerTy() && target->isPointerTy())
            return st_.builder->CreatePointerCast(v, target, "ptrcast");
        return v;
    }

    // ── Optimisation passes ───────────────────────────────────────────
    void optimise() {
        if (opts_.opt == OptLevel::Debug) return;
        PassBuilder pb;
        LoopAnalysisManager     lam;
        FunctionAnalysisManager fam;
        CGSCCAnalysisManager    cgam;
        ModuleAnalysisManager   mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        OptimizationLevel level;
        switch (opts_.opt) {
            case OptLevel::ReleaseSafe:  level = OptimizationLevel::O2; break;
            case OptLevel::ReleaseFast:  level = OptimizationLevel::O3; break;
            case OptLevel::ReleaseSmall: level = OptimizationLevel::Os; break;
            default:                     level = OptimizationLevel::O2; break;
        }
        ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
        mpm.run(*st_.mod, mam);
    }
};

/* ── C-linkage API (called from rsc/main.c) ───────────────────────── */
extern "C" {

void *llvm_backend_create(DiagSink *diags, int opt_level, const char *triple,
                           const char *cpu, int emit_ir) {
    CodegenOptions opts;
    opts.opt      = (OptLevel)opt_level;
    opts.triple   = triple ? triple : "";
    opts.cpu      = cpu ? cpu : "generic";
    opts.emit_ir  = emit_ir != 0;
    return new Backend(diags, opts);
}

int llvm_backend_compile(void *backend, const AstFile *ast, const char *out_path) {
    Backend *b = static_cast<Backend*>(backend);
    return b->compile_file(ast, out_path) ? 0 : 1;
}

const char *llvm_backend_emit_ir(void *backend, const AstFile *ast, Arena *arena) {
    Backend *b = static_cast<Backend*>(backend);
    std::string ir = b->emit_ir_string(ast);
    char *buf = (char*)arena_alloc(arena, ir.size()+1, 1);
    memcpy(buf, ir.c_str(), ir.size()+1);
    return buf;
}

void llvm_backend_destroy(void *backend) {
    delete static_cast<Backend*>(backend);
}

} // extern "C"

} // namespace codegen
} // namespace rsharp

#endif // RSHARP_WITH_LLVM
