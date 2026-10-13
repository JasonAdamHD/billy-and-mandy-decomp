# Review pass (improve weak names)

Each function in the batch already has a provisional name (shown after the address, possibly `Class::Method`),
produced by a weaker model with less context. Callers/callees and strings are now mostly named, which gives
much better context. For EVERY function:
- Decide the best name per NAMING_RULES.md (same format and conventions).
- If the provisional name is already good, output it unchanged.
- Replace placeholders (vVirtualN, Unknown*, *Stub*, Get/SetField<hex>, *Global<hex>, Func<hex>) with a
  descriptive name derived from what the code does and how callers use it.
- Keep the class part unless clearly wrong; never invent a class (see NAMING_RULES.md).
- Drop trailing `_2`/`_3` dedup suffixes; the apply tool re-adds them only if needed.
Output CSV lines: 0xADDRESS,Name,confidence (h/m/l) — one per function, nothing else.
