# Reverse-engineering pipeline

Output of this pipeline is derived from the original executable and is
**not committed** (it goes to `re/work/`, which is git-ignored).

## 1. Export the executable

```
ledump ASCEND.EXE --elf re/work/ascend.elf --fixups re/work/fixups.txt
```

## 2. Teach Ghidra the Watcom calling convention

Ghidra (tested with 11.4.2) has no `__watcall`. In
`Ghidra/Processors/x86/data/languages/x86gcc.cspec`:

1. Replace the `<default_proto>` with:

```xml
<default_proto>
  <prototype name="__watcall" extrapop="unknown" stackshift="4">
    <input>
      <pentry minsize="1" maxsize="4"><register name="EAX"/></pentry>
      <pentry minsize="1" maxsize="4"><register name="EDX"/></pentry>
      <pentry minsize="1" maxsize="4"><register name="EBX"/></pentry>
      <pentry minsize="1" maxsize="4"><register name="ECX"/></pentry>
      <pentry minsize="1" maxsize="500" align="4"><addr offset="4" space="stack"/></pentry>
    </input>
    <output killedbycall="true">
      <pentry minsize="4" maxsize="10" metatype="float" extension="float"><register name="ST0"/></pentry>
      <pentry minsize="1" maxsize="4"><register name="EAX"/></pentry>
      <pentry minsize="5" maxsize="8"><addr space="join" piece1="EDX" piece2="EAX"/></pentry>
    </output>
    <unaffected>
      <register name="ESP"/><register name="EBP"/><register name="ESI"/><register name="EDI"/>
      <register name="EBX"/><register name="ECX"/><register name="EDX"/>
    </unaffected>
    <killedbycall><register name="ST0"/><register name="ST1"/></killedbycall>
  </prototype>
</default_proto>
```

   and keep the old `__cdecl` prototype as a normal `<prototype>`.
2. Delete the `__regparm1/2/3` prototypes.
3. Replace the `resolveprototype`/`eval_current_prototype` pair with:

```xml
<resolveprototype name="__watcall/__cdecl">
  <model name="__watcall"/>
  <model name="__cdecl"/>
</resolveprototype>
<eval_current_prototype name="__watcall/__cdecl"/>
```

## 3. Analyse and export

```
analyzeHeadless re/work/proj ascend -import re/work/ascend.elf -overwrite \
  -scriptPath re/ghidra_scripts \
  -preScript Prepare.java re/work/fixups.txt \
  -postScript ExportAll.java re/work/out
```

Result: ~1,420 functions (227 auto-named by source module, e.g.
`cosmos_0002f3a0`), `functions.tsv`, `strings.tsv`, `decomp.c`.
