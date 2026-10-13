# Function naming rules (PS2 game: "The Grim Adventures of Billy & Mandy", C++ engine)

Input: a batch file of decompiled functions (Ghidra C). For EVERY function in it, propose a name.

Engine conventions (from the game's own strings):
- Classes are prefixed by module: At* = engine (AtFp = Flash player, AtIo = file/IO, AtMa = math, AtBa = base,
  AtCi = cinema, AtCo = controller, AtVp = video player, AtAp = application), Gm* = game layer
  (GmAc = actors, GmBa = base, GmBe = behaviour, GmCi = cinema, GmCo = controller).
- Methods use Hungarian return-type prefixes: v=void, b=bool, n=int, f=float, po=pointer to object,
  p=pointer, e=enum, s=string. E.g. AtFpDisplayManager::vSetTarget, AtFpSprite::poGetFrame, AtIoWinFileSystem::eOpen.
- Source file "Foo.cpp" normally holds class Foo. The `file=` line tells you which file the function sits in
  ("inside X.cpp"/"direct" = certain; "after A / before B" = it is in A, B, or a file between them).
- Functions sharing a `vtable: VT_xxxx[i/n]` are virtual methods of the same class; slot 0/1 is often the destructor.

Name format:
- Member function: `ClassName::vMethodName` (use the class from the file name when the code fits).
- Free/static function: `PascalCaseVerbNoun` (e.g. `RotateVector2D`), optionally module-prefixed (`AtMa_...`).
- Constructor: `ClassName::ClassName`; destructor: `ClassName::Destructor`.
- Names must be valid C identifiers (only [A-Za-z0-9_] plus one optional `ClassName::`). No templates/operators.
- Do not reuse a name already used for a different address in the same batch; add a meaningful distinguisher
  (or `_2` only as a last resort).
- VU0 macro ops (_vmul, _lqc2, ...) mean vector/matrix math (4-float vectors).

Output: write a CSV file (no header) with one line per function:
  0xADDRESS,Name,confidence
confidence: h = clear from strings/callees/structure; m = reasonable inference; l = guess from shape only.
Even for `l`, give the most descriptive name you can (e.g. `GmAcActor::vUnknownVirtual3` is acceptable only if
nothing better can be inferred). Never skip a function. Do not output anything else in the file.

Extra rules (important):
- NEVER invent a class name. Only use a class that appears in the `file=` line, in a string, or in an existing
  `Class::` name among callers/callees. If unsure which class, use a free-function name instead.
- Prefer a descriptive method name (e.g. `vBeginDisplayList`) over placeholders like `vVirtual3`; use a
  placeholder only when the body gives no hint at all, and then mark it `l`.
- Read the whole batch file once (use offset/limit chunks of ~600 lines), then write the CSV once.
