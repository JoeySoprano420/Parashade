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

