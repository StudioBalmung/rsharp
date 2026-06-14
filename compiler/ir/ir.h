/* rsharp/compiler/ir/ir.h — R# High-Level IR (between sema and LLVM)
 *
 * This IR is simpler than LLVM IR and closer to R# semantics.
 * It makes ownership/lifetime analysis easier before lowering to LLVM.
 *
 * Status: interface defined, lowering pass is a v1.1 milestone.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../parser/ast.h"
#include "../sema/sema.h"

typedef enum IrOpcode {
    /* Constants */
    IR_CONST_INT, IR_CONST_FLOAT, IR_CONST_BOOL, IR_CONST_NULL,
    /* Variables */
    IR_ALLOCA,      /* allocate local variable slot     */
    IR_LOAD,        /* load from slot/ptr               */
    IR_STORE,       /* store to slot/ptr                */
    IR_MOVE,        /* ownership move                   */
    IR_BORROW,      /* shared borrow                    */
    IR_BORROW_MUT,  /* mutable borrow                   */
    IR_DROP,        /* explicit drop / scope end        */
    /* Arithmetic */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_NEG, IR_NOT, IR_BITNOT,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
    /* Comparison */
    IR_EQ, IR_NEQ, IR_LT, IR_GT, IR_LE, IR_GE,
    /* Control flow */
    IR_JUMP,        /* unconditional branch             */
    IR_BRANCH,      /* conditional branch               */
    IR_CALL,        /* function call                    */
    IR_RETURN,
    IR_DEFER_PUSH,  /* push deferred action             */
    IR_DEFER_RUN,   /* run deferred actions (LIFO)      */
    /* Memory */
    IR_INDEX,       /* slice/array index                */
    IR_FIELD,       /* struct field access              */
    IR_CAST,        /* type cast                        */
    IR_SIZEOF,      /* sizeof type                      */
} IrOpcode;

typedef uint32_t IrReg;   /* virtual register ID */
typedef uint32_t IrBlock; /* basic block ID       */

typedef struct IrInstr {
    IrOpcode      op;
    IrReg         dst;       /* destination register (0 = void) */
    IrReg         src1, src2;
    ResolvedType *type;
    union {
        int64_t  int_val;
        double   flt_val;
        IrBlock  target_block;
        struct { IrReg *args; size_t argc; } call;
        struct { uint32_t field_idx; } field;
    };
} IrInstr;

typedef struct IrBasicBlock {
    IrBlock  id;
    IrInstr *instrs;
    size_t   count, cap;
    IrBlock  succs[2];   /* successor blocks (branch targets) */
} IrBasicBlock;

typedef struct IrFn {
    const char   *name;
    IrBasicBlock *blocks;
    size_t        block_count;
    uint32_t      reg_count;
    ResolvedType *ret_type;
} IrFn;

typedef struct IrModule {
    const char *name;
    IrFn       *fns;
    size_t      fn_count;
} IrModule;
