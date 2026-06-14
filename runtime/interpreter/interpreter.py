"""
rsharp/runtime/interpreter/interpreter.py

R# Tree-walking Interpreter — used for:
  - REPL (`rss repl`)
  - Quick script execution without full LLVM compile
  - Bootstrapping stage before self-hosting

This interpreter is intentionally simple: it walks the AST (fed from
the C parser via ctypes bindings) and executes nodes directly.
No JIT, no bytecode — pure Python for rapid iteration.

When the C compiler+LLVM backend is complete, this file becomes
a dev tool only (tests, fuzzing, playground).
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Any, Optional, Dict, List, Callable
import math
import sys
import os

# ──────────────────────────────────────────────────────────────────────────────
# Values
# ──────────────────────────────────────────────────────────────────────────────

class RsharpError(Exception):
    """Runtime error in R# code (not a Python bug)."""
    def __init__(self, msg: str, span=None):
        super().__init__(msg)
        self.msg  = msg
        self.span = span

@dataclass
class RSNull:
    def __repr__(self): return "null"

@dataclass
class RSResult:
    ok:    bool
    value: Any   # the Ok value OR the Err value

@dataclass
class RSStruct:
    type_name: str
    fields:    Dict[str, Any]
    def __repr__(self): return f"{self.type_name} {{ {', '.join(f'{k}: {v!r}' for k,v in self.fields.items())} }}"

@dataclass
class RSEnum:
    variant: str
    payload: Dict[str, Any]
    def __repr__(self): return f".{self.variant}"

@dataclass
class RSSlice:
    data:   List[Any]
    start:  int = 0
    length: int = -1  # -1 means full list
    def __len__(self): return self.length if self.length >= 0 else len(self.data)
    def __getitem__(self, i):
        if i < 0 or i >= len(self): raise RsharpError(f"slice index {i} out of bounds (len={len(self)})")
        return self.data[self.start + i]

@dataclass
class RSFunction:
    name:   str
    params: List[str]
    body:   Any          # AST node (dict)
    env:    "Environment"

@dataclass
class RSBuiltinFn:
    name: str
    fn:   Callable

NULL = RSNull()

# ──────────────────────────────────────────────────────────────────────────────
# Environment (scope chain)
# ──────────────────────────────────────────────────────────────────────────────

class Environment:
    def __init__(self, parent: Optional["Environment"] = None):
        self.vars:   Dict[str, Any]  = {}
        self.consts: Dict[str, Any]  = {}
        self.parent  = parent

    def get(self, name: str) -> Any:
        if name in self.vars:   return self.vars[name]
        if name in self.consts: return self.consts[name]
        if self.parent:         return self.parent.get(name)
        raise RsharpError(f"undefined variable '{name}'")

    def set(self, name: str, value: Any):
        if name in self.consts:
            raise RsharpError(f"cannot assign to const '{name}'")
        if name in self.vars:
            self.vars[name] = value
            return
        if self.parent and self.parent._has(name):
            self.parent.set(name, value)
            return
        raise RsharpError(f"undefined variable '{name}' (use 'let' or 'var' to declare)")

    def define(self, name: str, value: Any, mutable: bool = True):
        if mutable: self.vars[name]   = value
        else:       self.consts[name] = value

    def _has(self, name: str) -> bool:
        return (name in self.vars or name in self.consts or
                (self.parent is not None and self.parent._has(name)))

# ──────────────────────────────────────────────────────────────────────────────
# Control flow signals (use Python exceptions as non-local jumps)
# ──────────────────────────────────────────────────────────────────────────────

class ReturnSignal(Exception):
    def __init__(self, value): self.value = value

class BreakSignal(Exception):
    def __init__(self, value=NULL): self.value = value

class ContinueSignal(Exception):
    pass

# ──────────────────────────────────────────────────────────────────────────────
# Interpreter
# ──────────────────────────────────────────────────────────────────────────────

class Interpreter:
    def __init__(self):
        self.globals = Environment()
        self._install_builtins()

    # ── Builtins ──────────────────────────────────────────────────────────────

    def _install_builtins(self):
        builtins = {
            "@print":  self._builtin_print,
            "@assert": self._builtin_assert,
            "@panic":  self._builtin_panic,
            "@sqrt":   lambda args: math.sqrt(args[0]),
            "@floor":  lambda args: math.floor(args[0]),
            "@ceil":   lambda args: math.ceil(args[0]),
            "@abs":    lambda args: abs(args[0]),
            "@min":    lambda args: min(args[0], args[1]),
            "@max":    lambda args: max(args[0], args[1]),
            "@len":    lambda args: len(args[0]) if hasattr(args[0], '__len__') else 0,
            "@typeof": lambda args: type(args[0]).__name__,
        }
        for name, fn in builtins.items():
            self.globals.define(name, RSBuiltinFn(name, fn), mutable=False)

    def _builtin_print(self, args):
        if not args: print(); return
        fmt = args[0] if isinstance(args[0], str) else str(args[0])
        vals = args[1:]
        out = fmt.replace("{}", "{}").format(*[self._display(v) for v in vals])
        sys.stdout.write(out + "\n")

    def _builtin_assert(self, args):
        cond = args[0]
        msg  = args[1] if len(args) > 1 else "assertion failed"
        if not cond: raise RsharpError(str(msg))

    def _builtin_panic(self, args):
        msg = args[0] if args else "explicit panic"
        raise RsharpError(str(msg))

    def _display(self, v) -> str:
        if v is None or isinstance(v, RSNull): return "null"
        if isinstance(v, bool):  return "true" if v else "false"
        if isinstance(v, float): return f"{v:g}"
        if isinstance(v, RSStruct): return repr(v)
        if isinstance(v, RSSlice):  return f"[{', '.join(self._display(x) for x in v.data[v.start:v.start+len(v)])}]"
        return str(v)

    # ── Top-level execution ───────────────────────────────────────────────────

    def exec_file(self, ast: dict):
        """Execute a parsed AstFile (represented as dict from C via JSON bridge)."""
        for decl in ast.get("decls", []):
            self.exec_decl(decl, self.globals)

    def exec_decl(self, decl: dict, env: Environment):
        kind = decl["kind"]
        if kind == "fn":
            fn = RSFunction(
                name=decl["name"],
                params=[p["name"] for p in decl.get("params", [])],
                body=decl["body"],
                env=env,
            )
            env.define(decl["name"], fn, mutable=False)
        elif kind == "const":
            val = self.eval_expr(decl["value"], env)
            env.define(decl["name"], val, mutable=False)
        elif kind == "struct":
            # Register struct constructor
            fields = [f["name"] for f in decl.get("fields", [])]
            name   = decl["name"]
            def make_ctor(n, flds):
                def ctor(args):
                    if len(args) != len(flds):
                        raise RsharpError(f"{n} expects {len(flds)} fields, got {len(args)}")
                    return RSStruct(n, dict(zip(flds, args)))
                return ctor
            env.define(name, RSBuiltinFn(name, make_ctor(name, fields)), mutable=False)

    # ── Statement execution ───────────────────────────────────────────────────

    def exec_stmts(self, stmts: list, env: Environment) -> Any:
        last = NULL
        for stmt in stmts:
            last = self.exec_stmt(stmt, env)
        return last

    def exec_stmt(self, stmt: dict, env: Environment) -> Any:
        kind = stmt["kind"]

        if kind in ("let", "var"):
            val = self.eval_expr(stmt["init"], env) if stmt.get("init") else NULL
            env.define(stmt["name"], val, mutable=(kind == "var"))
            return NULL

        elif kind == "return":
            val = self.eval_expr(stmt["value"], env) if stmt.get("value") else NULL
            raise ReturnSignal(val)

        elif kind == "break":
            val = self.eval_expr(stmt["value"], env) if stmt.get("value") else NULL
            raise BreakSignal(val)

        elif kind == "continue":
            raise ContinueSignal()

        elif kind == "defer":
            # Defer is handled at the block level; collect into a list
            env.define("__deferred__",
                       env.vars.get("__deferred__", []) + [stmt["inner"]],
                       mutable=True)
            return NULL

        elif kind == "expr_stmt":
            return self.eval_expr(stmt["expr"], env)

        elif kind == "for":
            iter_val = self.eval_expr(stmt["iter"], env)
            items    = self._iterate(iter_val)
            result   = NULL
            loop_env = Environment(env)
            for item in items:
                loop_env.define(stmt["bind"], item, mutable=True)
                try:
                    self.exec_stmts(stmt["body"], loop_env)
                except BreakSignal as b:
                    result = b.value; break
                except ContinueSignal:
                    continue
            return result

        elif kind == "while":
            while self.eval_expr(stmt["cond"], env):
                try:
                    self.exec_stmts(stmt["body"], Environment(env))
                except BreakSignal as b:
                    return b.value
                except ContinueSignal:
                    continue
            return NULL

        elif kind == "assign":
            val = self.eval_expr(stmt["rhs"], env)
            self._assign(stmt["lhs"], val, env)
            return NULL

        else:
            raise RsharpError(f"[interpreter] unhandled stmt kind: {kind}")

    def _iterate(self, val: Any) -> List[Any]:
        if isinstance(val, RSSlice):
            return [val[i] for i in range(len(val))]
        if isinstance(val, list):
            return val
        if isinstance(val, dict) and "start" in val:
            start = val["start"]; stop = val["end"]
            return list(range(start, stop + 1 if val.get("inclusive") else stop))
        if isinstance(val, range):
            return list(val)
        raise RsharpError(f"value of type {type(val).__name__} is not iterable")

    def _assign(self, target: dict, value: Any, env: Environment):
        kind = target.get("kind")
        if kind == "ident":
            env.set(target["name"], value)
        elif kind == "index":
            obj = self.eval_expr(target["obj"], env)
            idx = self.eval_expr(target["idx"], env)
            obj.data[obj.start + idx] = value
        elif kind == "field":
            obj = self.eval_expr(target["obj"], env)
            if isinstance(obj, RSStruct):
                obj.fields[target["field"]] = value

    # ── Expression evaluation ─────────────────────────────────────────────────

    def eval_expr(self, expr: dict, env: Environment) -> Any:
        kind = expr["kind"]

        if kind == "int_lit":   return expr["value"]
        if kind == "float_lit": return float(expr["value"])
        if kind == "bool_lit":  return expr["value"]
        if kind == "str_lit":   return expr["value"]
        if kind == "null":      return NULL

        if kind == "ident":
            return env.get(expr["name"])

        if kind == "block":
            block_env = Environment(env)
            block_env.define("__deferred__", [], mutable=True)
            result = NULL
            try:
                self.exec_stmts(expr.get("stmts", []), block_env)
                if expr.get("tail"):
                    result = self.eval_expr(expr["tail"], block_env)
            finally:
                # Run deferred statements in LIFO order
                for d in reversed(block_env.vars.get("__deferred__", [])):
                    try:
                        self.exec_stmt(d, block_env)
                    except Exception:
                        pass
            return result

        if kind == "if":
            cond = self.eval_expr(expr["cond"], env)
            if cond:
                return self.eval_expr(expr["then"], env)
            elif expr.get("else"):
                return self.eval_expr(expr["else"], env)
            return NULL

        if kind == "match":
            subject = self.eval_expr(expr["subject"], env)
            for arm in expr["arms"]:
                if self._match_pattern(arm["pattern"], subject, env):
                    arm_env = Environment(env)
                    self._bind_pattern(arm["pattern"], subject, arm_env)
                    if arm.get("guard") and not self.eval_expr(arm["guard"], arm_env):
                        continue
                    return self.eval_expr(arm["body"], arm_env)
            raise RsharpError("non-exhaustive match — no arm matched")

        if kind == "call":
            fn  = self.eval_expr(expr["callee"], env)
            args = [self.eval_expr(a, env) for a in expr.get("args", [])]
            return self._call(fn, args)

        if kind == "field":
            obj = self.eval_expr(expr["obj"], env)
            return self._field_access(obj, expr["field"])

        if kind == "index":
            obj = self.eval_expr(expr["obj"], env)
            idx = self.eval_expr(expr["idx"], env)
            if isinstance(idx, dict):
                # Range slice
                start = idx.get("start", 0)
                end   = idx.get("end", len(obj))
                return RSSlice(obj.data, obj.start + start, end - start)
            return obj[idx]

        if kind == "unary":
            v = self.eval_expr(expr["operand"], env)
            op = expr["op"]
            if op == "neg":    return -v
            if op == "not":    return not v
            if op == "bitnot": return ~v
            if op == "deref":  return v  # simplified: no real pointer deref
            if op == "addr":   return v  # simplified: address-of

        if kind == "binary":
            op  = expr["op"]
            lhs = self.eval_expr(expr["lhs"], env)
            rhs = self.eval_expr(expr["rhs"], env)
            return self._binop(op, lhs, rhs)

        if kind == "range":
            return {"start": self.eval_expr(expr["start"], env),
                    "end":   self.eval_expr(expr["end"], env),
                    "inclusive": expr.get("inclusive", False)}

        if kind == "catch":
            result = self.eval_expr(expr["expr"], env)
            if isinstance(result, RSResult) and not result.ok:
                handler_env = Environment(env)
                handler_env.define(expr["err_bind"], result.value, mutable=False)
                return self.eval_expr(expr["handler"], handler_env)
            return result.value if isinstance(result, RSResult) else result

        if kind == "try":
            result = self.eval_expr(expr["expr"], env)
            if isinstance(result, RSResult) and not result.ok:
                raise ReturnSignal(result)
            return result.value if isinstance(result, RSResult) else result

        if kind == "nullcoal":
            v = self.eval_expr(expr["lhs"], env)
            return self.eval_expr(expr["rhs"], env) if isinstance(v, RSNull) else v

        if kind == "builtin":
            name = expr["name"]
            args = [self.eval_expr(a, env) for a in expr.get("args", [])]
            fn = env.get(name)
            if isinstance(fn, RSBuiltinFn):
                return fn.fn(args)
            raise RsharpError(f"unknown builtin {name}")

        raise RsharpError(f"[interpreter] unhandled expr kind: {kind}")

    def _binop(self, op: str, lhs: Any, rhs: Any) -> Any:
        ops = {
            "add": lambda a,b: a+b, "sub": lambda a,b: a-b,
            "mul": lambda a,b: a*b, "div": lambda a,b: a/b,
            "mod": lambda a,b: a%b,
            "eq":  lambda a,b: a==b, "neq": lambda a,b: a!=b,
            "lt":  lambda a,b: a<b,  "gt":  lambda a,b: a>b,
            "le":  lambda a,b: a<=b, "ge":  lambda a,b: a>=b,
            "and": lambda a,b: a and b, "or": lambda a,b: a or b,
            "bitand": lambda a,b: a&b, "bitor": lambda a,b: a|b,
            "bitxor": lambda a,b: a^b,
            "shl": lambda a,b: a<<b, "shr": lambda a,b: a>>b,
            "nullcoal": lambda a,b: b if isinstance(a, RSNull) else a,
        }
        if op not in ops:
            raise RsharpError(f"unknown binary operator: {op}")
        return ops[op](lhs, rhs)

    def _call(self, fn: Any, args: List[Any]) -> Any:
        if isinstance(fn, RSBuiltinFn):
            return fn.fn(args)
        if isinstance(fn, RSFunction):
            fn_env = Environment(fn.env)
            for param, arg in zip(fn.params, args):
                fn_env.define(param, arg, mutable=True)
            try:
                self.exec_stmts(fn.body, fn_env)
                return NULL
            except ReturnSignal as r:
                return r.value
        raise RsharpError(f"not callable: {type(fn).__name__}")

    def _field_access(self, obj: Any, field: str) -> Any:
        if isinstance(obj, RSStruct):
            if field in obj.fields: return obj.fields[field]
            raise RsharpError(f"struct '{obj.type_name}' has no field '{field}'")
        if isinstance(obj, RSSlice):
            if field == "len": return len(obj)
            if field == "ptr": return obj.data
        if isinstance(obj, RSEnum):
            if field in obj.payload: return obj.payload[field]
        raise RsharpError(f"cannot access field '{field}' on {type(obj).__name__}")

    def _match_pattern(self, pattern: dict, subject: Any, env: Environment) -> bool:
        kind = pattern.get("kind")
        if kind == "wildcard":   return True
        if kind == "int_lit":    return subject == pattern["value"]
        if kind == "bool_lit":   return subject == pattern["value"]
        if kind == "null":       return isinstance(subject, RSNull)
        if kind == "enum_path":
            return isinstance(subject, RSEnum) and subject.variant == pattern["variant"]
        if kind == "binding":    return True
        return False

    def _bind_pattern(self, pattern: dict, subject: Any, env: Environment):
        kind = pattern.get("kind")
        if kind == "binding":
            env.define(pattern["name"], subject, mutable=False)
        elif kind == "enum_path" and isinstance(subject, RSEnum):
            for k, v in subject.payload.items():
                env.define(k, v, mutable=False)

# ──────────────────────────────────────────────────────────────────────────────
# REPL
# ──────────────────────────────────────────────────────────────────────────────

def repl():
    """Interactive R# REPL (uses interpreter + a minimal inline parser)."""
    interp = Interpreter()
    print("R# REPL v0.1 — type 'exit' to quit\n")
    while True:
        try:
            line = input("rss> ").strip()
        except (EOFError, KeyboardInterrupt):
            print(); break
        if line in ("exit", "quit", ":q"): break
        if not line: continue

        # For now, only simple expressions (full parser is in C)
        # This stub prints a placeholder; replace with ctypes bridge
        print(f"  [interpreter] expression received: {line!r}")
        print("  (Full AST parser bridge not yet connected — run `rsc` for compilation)")

if __name__ == "__main__":
    repl()
