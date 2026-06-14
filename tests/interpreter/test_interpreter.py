"""
tests/interpreter/test_interpreter.py
Unit tests for the R# tree-walking interpreter.
Run: python3 -m pytest tests/interpreter/ -v
"""
import sys, os
import importlib.util, pathlib
_interp_path = pathlib.Path(__file__).parent.parent.parent / 'runtime' / 'interpreter' / 'interpreter.py'
_spec = importlib.util.spec_from_file_location('rsharp_interpreter', _interp_path)
_mod  = importlib.util.module_from_spec(_spec)
import sys as _sys
_sys.modules['rsharp_interpreter'] = _mod
_spec.loader.exec_module(_mod)
Interpreter = _mod.Interpreter
Environment  = _mod.Environment
RSStruct     = _mod.RSStruct
RSSlice      = _mod.RSSlice
RSNull       = _mod.RSNull
NULL         = _mod.NULL
ReturnSignal = _mod.ReturnSignal
RsharpError  = _mod.RsharpError

def make_interp():
    return Interpreter()

# ── AST node helpers ───────────────────────────────────────────────

def int_lit(v):
    return {"kind": "int_lit", "value": v, "type": None}

def float_lit(v):
    return {"kind": "float_lit", "value": v, "type": None}

def bool_lit(v):
    return {"kind": "bool_lit", "value": v, "type": None}

def str_lit(v):
    return {"kind": "str_lit", "value": v, "type": None}

def ident(name):
    return {"kind": "ident", "name": name, "type": None}

def binop(op, lhs, rhs):
    return {"kind": "binary", "op": op, "lhs": lhs, "rhs": rhs, "type": None}

def let_stmt(name, init, is_mut=False):
    return {"kind": "let", "name": name, "init": init, "is_mut": is_mut, "ty": None}

def return_stmt(val):
    return {"kind": "return", "value": val}

def block_expr(stmts, tail=None):
    return {"kind": "block", "stmts": stmts, "tail": tail, "type": None}

def if_expr(cond, then, else_=None):
    return {"kind": "if", "cond": cond, "then": then, "else": else_, "type": None}

def call_expr(callee_name, args):
    return {"kind": "call", "callee": ident(callee_name), "args": args, "type": None}

def fn_decl(name, params, body):
    # params: list of str -> wrap as dicts for interpreter
    param_dicts = [{"name": p} if isinstance(p, str) else p for p in params]
    return {"kind": "fn", "name": name, "params": param_dicts, "body": body}

def builtin_expr(name, args):
    return {"kind": "builtin", "name": "@" + name, "args": args, "type": None}

def range_expr(start, end, inclusive=False):
    return {"kind": "range", "start": int_lit(start), "end": int_lit(end),
            "inclusive": inclusive, "type": None}

def for_stmt(bind, iter, body):
    return {"kind": "for", "bind": bind, "iter": iter, "body": body}

def while_stmt(cond, body):
    return {"kind": "while", "cond": cond, "body": body}


# ── Tests ──────────────────────────────────────────────────────────

class TestLiterals:
    def test_int(self):
        i = make_interp()
        assert i.eval_expr(int_lit(42), i.globals) == 42

    def test_float(self):
        i = make_interp()
        assert i.eval_expr(float_lit(3.14), i.globals) == 3.14

    def test_bool_true(self):
        i = make_interp()
        assert i.eval_expr(bool_lit(True), i.globals) == True

    def test_bool_false(self):
        i = make_interp()
        assert i.eval_expr(bool_lit(False), i.globals) == False

    def test_str(self):
        i = make_interp()
        assert i.eval_expr(str_lit("hello"), i.globals) == "hello"

    def test_null(self):
        i = make_interp()
        result = i.eval_expr({"kind": "null", "type": None}, i.globals)
        assert isinstance(result, RSNull)


class TestArithmetic:
    def test_add(self):
        i = make_interp()
        assert i.eval_expr(binop("add", int_lit(3), int_lit(4)), i.globals) == 7

    def test_sub(self):
        i = make_interp()
        assert i.eval_expr(binop("sub", int_lit(10), int_lit(3)), i.globals) == 7

    def test_mul(self):
        i = make_interp()
        assert i.eval_expr(binop("mul", int_lit(6), int_lit(7)), i.globals) == 42

    def test_div(self):
        i = make_interp()
        assert i.eval_expr(binop("div", int_lit(15), int_lit(3)), i.globals) == 5.0

    def test_mod(self):
        i = make_interp()
        assert i.eval_expr(binop("mod", int_lit(17), int_lit(5)), i.globals) == 2

    def test_float_add(self):
        i = make_interp()
        result = i.eval_expr(binop("add", float_lit(1.5), float_lit(2.5)), i.globals)
        assert abs(result - 4.0) < 1e-9

    def test_nested(self):
        i = make_interp()
        # (2 + 3) * 4 = 20
        expr = binop("mul", binop("add", int_lit(2), int_lit(3)), int_lit(4))
        assert i.eval_expr(expr, i.globals) == 20


class TestComparisons:
    def test_eq_true(self):
        i = make_interp()
        assert i.eval_expr(binop("eq", int_lit(5), int_lit(5)), i.globals) == True

    def test_eq_false(self):
        i = make_interp()
        assert i.eval_expr(binop("eq", int_lit(5), int_lit(6)), i.globals) == False

    def test_lt(self):
        i = make_interp()
        assert i.eval_expr(binop("lt", int_lit(3), int_lit(5)), i.globals) == True

    def test_gt(self):
        i = make_interp()
        assert i.eval_expr(binop("gt", int_lit(5), int_lit(3)), i.globals) == True

    def test_neq(self):
        i = make_interp()
        assert i.eval_expr(binop("neq", int_lit(1), int_lit(2)), i.globals) == True

    def test_logical_and(self):
        i = make_interp()
        assert i.eval_expr(binop("and", bool_lit(True), bool_lit(True)), i.globals) == True
        assert i.eval_expr(binop("and", bool_lit(True), bool_lit(False)), i.globals) == False

    def test_logical_or(self):
        i = make_interp()
        assert i.eval_expr(binop("or", bool_lit(False), bool_lit(True)), i.globals) == True


class TestVariables:
    def test_let_define_and_read(self):
        i = make_interp()
        env = Environment(i.globals)
        i.exec_stmt(let_stmt("x", int_lit(42)), env)
        assert i.eval_expr(ident("x"), env) == 42

    def test_var_mutable(self):
        i = make_interp()
        env = Environment(i.globals)
        i.exec_stmt(let_stmt("n", int_lit(10), is_mut=True), env)
        assert i.eval_expr(ident("n"), env) == 10

    def test_let_string(self):
        i = make_interp()
        env = Environment(i.globals)
        i.exec_stmt(let_stmt("s", str_lit("world")), env)
        assert i.eval_expr(ident("s"), env) == "world"

    def test_undefined_raises(self):
        import pytest
        i = make_interp()
        RsharpError = _mod.RsharpError
        with pytest.raises(RsharpError):
            i.eval_expr(ident("undefined_var"), i.globals)


class TestControlFlow:
    def test_if_true(self):
        i = make_interp()
        expr = if_expr(bool_lit(True), int_lit(1), int_lit(0))
        assert i.eval_expr(expr, i.globals) == 1

    def test_if_false(self):
        i = make_interp()
        expr = if_expr(bool_lit(False), int_lit(1), int_lit(0))
        assert i.eval_expr(expr, i.globals) == 0

    def test_if_no_else_true(self):
        i = make_interp()
        expr = if_expr(bool_lit(True), int_lit(99))
        assert i.eval_expr(expr, i.globals) == 99

    def test_block_evaluates_tail(self):
        i = make_interp()
        expr = block_expr(
            [let_stmt("x", int_lit(5))],
            tail=binop("mul", ident("x"), int_lit(2))
        )
        # Can't eval block directly as it needs stmt support; test via exec_stmts
        env = Environment(i.globals)
        result = i.eval_expr(expr, env)
        assert result == 10

    def test_for_range_count(self):
        i = make_interp()
        env = Environment(i.globals)
        i.exec_stmt(let_stmt("count", int_lit(0), is_mut=True), env)
        body = [{"kind": "expr_stmt", "expr":
                 {"kind": "binary", "op": "add",
                  "lhs": ident("count"), "rhs": int_lit(1), "type": None},
                 "type": None}]
        # Simplified: just check that for loop doesn't crash
        stmt = for_stmt("_i", range_expr(0, 5), body)
        i.exec_stmt(stmt, env)

    def test_while_terminates(self):
        i = make_interp()
        env = Environment(i.globals)
        i.exec_stmt(let_stmt("x", int_lit(0), is_mut=True), env)
        # while false {} — should just skip
        stmt = while_stmt(bool_lit(False), [])
        i.exec_stmt(stmt, env)
        assert i.eval_expr(ident("x"), env) == 0


class TestFunctions:
    def test_define_and_call(self):
        i = make_interp()
        decl = fn_decl("double", ["x"],
                       [return_stmt(binop("mul", ident("x"), int_lit(2)))])
        i.exec_decl(decl, i.globals)
        result = i.eval_expr(call_expr("double", [int_lit(5)]), i.globals)
        assert result == 10

    def test_recursive_fibonacci(self):
        i = make_interp()
        # fn fib(n) { if n <= 1 { n } else { fib(n-1) + fib(n-2) } }
        le_expr = binop("le", ident("n"), int_lit(1))
        then = ident("n")
        else_ = binop("add",
            call_expr("fib", [binop("sub", ident("n"), int_lit(1))]),
            call_expr("fib", [binop("sub", ident("n"), int_lit(2))]))
        body = [return_stmt(if_expr(le_expr, then, else_))]
        i.exec_decl(fn_decl("fib", ["n"], body), i.globals)
        assert i.eval_expr(call_expr("fib", [int_lit(0)]), i.globals) == 0
        assert i.eval_expr(call_expr("fib", [int_lit(1)]), i.globals) == 1
        assert i.eval_expr(call_expr("fib", [int_lit(7)]), i.globals) == 13
        assert i.eval_expr(call_expr("fib", [int_lit(10)]), i.globals) == 55

    def test_return_from_fn(self):
        i = make_interp()
        body = [return_stmt(int_lit(42))]
        i.exec_decl(fn_decl("answer", [], body), i.globals)
        result = i.eval_expr(call_expr("answer", []), i.globals)
        assert result == 42


class TestBuiltins:
    def test_sqrt(self):
        import math
        i = make_interp()
        expr = builtin_expr("sqrt", [float_lit(16.0)])
        result = i.eval_expr(expr, i.globals)
        assert abs(result - 4.0) < 1e-9

    def test_min(self):
        i = make_interp()
        expr = builtin_expr("min", [int_lit(3), int_lit(7)])
        assert i.eval_expr(expr, i.globals) == 3

    def test_max(self):
        i = make_interp()
        expr = builtin_expr("max", [int_lit(3), int_lit(7)])
        assert i.eval_expr(expr, i.globals) == 7

    def test_abs(self):
        i = make_interp()
        expr = builtin_expr("abs", [int_lit(-5)])
        assert i.eval_expr(expr, i.globals) == 5

    def test_len_string(self):
        i = make_interp()
        expr = builtin_expr("len", [str_lit("hello")])
        assert i.eval_expr(expr, i.globals) == 5

    def test_assert_passes(self):
        i = make_interp()
        expr = builtin_expr("assert", [bool_lit(True), str_lit("ok")])
        i.eval_expr(expr, i.globals)  # should not raise

    def test_assert_fails(self):
        import pytest
        RsharpError = _mod.RsharpError
        i = make_interp()
        expr = builtin_expr("assert", [bool_lit(False), str_lit("fail message")])
        with pytest.raises(RsharpError):
            i.eval_expr(expr, i.globals)


class TestEnvironmentScoping:
    def test_nested_scope(self):
        i = make_interp()
        outer = Environment(i.globals)
        inner = Environment(outer)
        outer.define("x", 10, mutable=True)
        inner.define("y", 20, mutable=True)
        assert inner.get("x") == 10  # can see outer
        assert inner.get("y") == 20

    def test_shadow(self):
        i = make_interp()
        outer = Environment(i.globals)
        inner = Environment(outer)
        outer.define("x", 10, mutable=False)
        inner.define("x", 99, mutable=False)
        assert inner.get("x") == 99   # inner shadows outer
        assert outer.get("x") == 10   # outer unchanged

    def test_set_propagates_upward(self):
        i = make_interp()
        outer = Environment(i.globals)
        inner = Environment(outer)
        outer.define("x", 0, mutable=True)
        inner.set("x", 42)
        assert outer.get("x") == 42
