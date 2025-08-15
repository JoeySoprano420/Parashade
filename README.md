# Parashade

Parashade — Founding Blueprint (v0.1)
0) North Star (what Parashade is)

Long-form superlative syntax: verbose, intention-revealing constructs (e.g., declare explicit integer named total equals greatest_of 0x20 and 0x11 end). These normalize to a compact “core dialect” before parsing.

C++ semantics: value semantics by default, strong static types, zero-cost abstractions (inline, constexpr-like lowering), RAII-style lifetimes.

Hexadecimal semantics: hex literals first-class (0x2A, 0x_ffff_0001), hex opcodes in the IR, and a hex packet format (.parx) for AOT binaries.

NASM grammar: line-oriented, label-driven, directive-friendly (no EBNF/ANTLR); think “assembler-style productions over lines/labels/ops”, then lower to a tiny hex IR.

Scope & Range paradigm: every block declares a scope (ownership/lifetime rules) and a range (resource & effect envelope). Ranges are like lifetimes/regions; they bound what a symbol can touch and when it ends.

AOT first, JIT by assertion: emit native/bytecode ahead of time; at runtime you can assert “frame-by-frame JIT” for hotspots (e.g., 16ms frame budgets) without line-by-line overhead.

Capsule-based memory: arenas (“capsules”) with typed handles; deterministic teardown at scope/range end.

Error containers (AOT quarantines): the compiler pre-allocates error containers (structured fallback paths & messages) so runtime never hard-stops; instead it downgrades, substitutes, or warns.

Flexible indents, strong static types: indentation is cosmetic; blocks use scope … end. Types are explicit by preference; contextual inference resolves the rare ambiguity.

Tracing via metadata serialization: the compiler emits a sidecar .meta.json for symbols, scopes/ranges, and code-address maps.

Runtime-contextual inference: warning engine uses live counters + metadata to surface gapless, context-aware warnings, and to trigger JIT-per-frame when assertions say so.

---

Superlatives (preprocessor maps to intrinsics):

greatest_of A and B → max(A,B)

least_of A and B → min(A,B)

ever_exact → constexpr-style fold

utterly_inline → strong inline request for zero-cost abstraction

swear_by_frame_jit → assert frame-JIT for the block

2) NASM-style line grammar (hand-rolled)

Lines: LABEL: | DIRECTIVE ARGS | INSN ARGS | KEYWORD …

Labels bind scopes/ranges or basic blocks.

Directives: .module, .scope, .range, .capsule, .meta, .jit.

Instructions: small core set for v0.1 (stack ops, arithmetic, loads/stores, call/ret).

Comments: ; to end of line.

Blocks end with end (not indentation).

3) IR and AOT packets

Hex IR (tiny stack machine):

01 vv vv vv vv vv vv vv vv PUSH_IMM64

02 ADD, 03 SUB, 04 MUL, 05 DIV

10 ii STORE_LOCAL (idx), 11 ii LOAD_LOCAL (idx)

20 ff CALL (func id), 21 RET

AOT file .parx: header (PARX, version), const pool, code segments (hex), symbol table, scope/range table.

Metadata .meta.json: scopes, ranges, var maps, address→source mapping, superlative fold logs.

4) Memory: Capsules

Capsule<T> = typed arena handle (monotonic bump alloc or slab), destroyed at range end.

Borrow rules are simple in v0.1: no aliasing across ranges; lend within same range only.

5) Error Containers (AOT quarantines)

Compiler emits per-site “containers” with:

code, explain, fallback (constant, recompute, or sentinel).

range_scope (where to log).

downgrade_to (warning severity if triggered).
Runtime never aborts; it consults the container and continues.

6) JIT-per-frame switching

Assertive switch: swear_by_frame_jit in source or --jit-frames=N.

Frame interpreter: executes a block for N frames; if hotspot persists (via counters), promote to optimized tier or stay in JIT.

Never line-by-line; budget is per frame (e.g., 16.6ms).

7) Tracing & warnings

Tracer serializes events to ring buffer and optionally to JSONL.

Runtime warning engine uses metadata + rolling counters to emit contextual messages without gaps.

A tiny, working reference (single-file C++17)

This is a minimal, compilable seed that:

Normalizes long-form ➜ core

Tokenizes NASM-like lines

Parses a micro-subset (module, scope…end, let, return, +)

Type-checks ints (explicit/implicit)

Emits a hex IR packet (to stdout) and a metadata blob

Runs the frame interpreter or AOT VM

Stubs capsules and error containers

---

