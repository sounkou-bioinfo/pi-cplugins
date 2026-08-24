# AGENTS

- Scope this repository: minimal `pi-cplugins` C ABI and a pure-C conformance fixture.
- The public C ABI contract is only `include/pi_plugin.h`; package exports define the JS/TS surface.
- Demonstrate the ABI through a Pi extension backed by TinyCC.
- Derive automatic binding metadata from Tree-sitter C; do not hand-maintain a second catalog.
- Validate changes with `make check`, C anti-slop review, and Linux/macOS CI.
