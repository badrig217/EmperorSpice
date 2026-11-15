"""Builds a symbol map of Game.exe 1.09 (MD5 7dc4fda2f6b7e8a6b70e846686a81938): function starts and names.

Only addresses, names and short descriptions are written - our own findings, no game code. Anyone with
their own Game.exe can rebuild the map and load it into a disassembler (tools/re/apply_symbols_*.py).

    python3 tools/re/symbols.py Game.exe docs/re [--with-strings]
      --with-strings: put the game's own log text into the note column (local use only, never commit:
                      the repo version keeps only names, addresses and our own notes)
      -> docs/re/game109-symbols.tsv   address, kind, name, source, note
         docs/re/game109-unittype.tsv  unit/building type struct (rules.txt keys -> field offsets)
         docs/re/game109-modules.tsv   source files of the game with their address ranges
         docs/re/coverage.txt          how much of the program has a name

Name sources (column "source"):
  manual   verified by hand (docs/reverse-engineering.md), see MANUAL below
  logstr   the function's own log/error message names it ("CUnitObject::Update, Invalid state ...")
  script   handler of a mission script command (function table 0x60a978, interpreter switch 0x4ef5d5)
  rules    parse handler of a rules.txt key (key table 0x61bff4, switch 0x5267a0)
  vtable   unit class virtual function with a known slot
  vtable   virtual function of a class whose name is known from another entry of its vtable or from the
           constructor's source file (named "Class::vf_<slot>")
  module   no own name, but the developers' source file is known ("Dune3\\Model\\unitHarvester.cpp"):
           from a source path the function references, or between two functions of the same file
           (the compiler lays out each source file's functions together)
Needs: pip install pefile capstone
"""
import bisect, collections, os, re, struct, sys
import pefile, capstone

BASE = 0x400000

# Verified by hand. Keep in sync with docs/reverse-engineering.md.
MANUAL = {
    0x401cb0: ('DebugPrintf', 'game log printf(fmt, ...); our PatchDebugLog routes it to emperor.txt'),
    0x401cc0: ('DebugPrintfError', 'log printf variant used for errors'),
    0x402f90: ('ResourceLibrary_ReadIndexes', 'reads PREFIX0001.RFH ... until the first gap'),
    0x403490: ('ResourceLibrary_Load', 'thiscall(name, uint32* size, 0, 0) -> file data'),
    0x403b70: ('ResourceLibrary_OpenData', 'opens the RFDs (whole file into one malloc block if flag set)'),
    0x417810: ('Texture_CreateFromData', 'cdecl(data, info)'),
    0x4ab330: ('Font_Load', '(mapPath, tgaPath, rowHeight, flag)'),
    0x4abcc0: ('MoveGroups_CreateFromOrder', 'thiscall on [world+0xabd8] (packet): group for >= 2 units'),
    0x4ad040: ('MoveGroups_FindPath', 'group-aware path search (shared usage grid)'),
    0x4c2e40: ('Order_BuildMove', 'builds a type 0 order packet; flag: execute now (AI) or queue'),
    0x4c3900: ('Order_Execute', 'cdecl(packet): executes an order (switch on type, table 0x4c5014)'),
    0x4e40e0: ('WorldToTile', '(out, vec3*) -> tile x/y'),
    0x4e41c0: ('TileToWorld', '(out, tile*) -> vec3 corner'),
    0x4e41f0: ('TileToWorldCentre', '(out, tile*) -> vec3 centre'),
    0x4e9940: ('MessageManager_Add', 'thiscall on [0x8aa5c4] (const char* text, 0, 0): in-game message line'),
    0x4f7c50: ('Model_BuildFileName', 'sprintf "%s%s_%c%c.xbf" (LOD letter, damage slot)'),
    0x4fc7a0: ('Model_DamageSlot', 'health ratio -> model slot H0/H1/H2/M3'),
    0x50a0f0: ('NetManager_RunFrame', 'thiscall on [0x7cb9d4]: frame++ (+0x14), executes queued orders'),
    0x50a5b0: ('NetManager_QueueOrder', 'thiscall on [0x7cb9d4] (packet)'),
    0x51caf0: ('Random_Next', 'synchronised game random generator (thiscall on [0x7d75dc])'),
    0x5344e0: ('Setup_GetGameType', 'thiscall on [0x7d75d4]; 3 = multiplayer'),
    0x5356a0: ('Setup_GetPlayerSide', 'thiscall on [0x7d75d4]'),
    0x53ebf0: ('Side_AddCash', 'thiscall(side, amount); cash at side+0x868'),
    0x53ec10: ('Side_SetCash', 'thiscall(side, amount)'),
    0x54c2b8: ('Texture_LoadByName', 'builds TEXTURES\\name, "Failed to find texture %s"'),
    0x54f680: ('Map_LoadTgaWash', '"Loading TGA Wash" (test.tga)'),
    0x55fe10: ('CUnitObject_Update', 'unit vtable +0x29c; switch on state [unit+0x38]'),
    0x562f00: ('CUnitObject_MoveStep', 'thiscall(arg), ret 4; 0 = arrived'),
    0x564d30: ('CUnitObject_GetSpeed', 'unit vtable +0x104; float in st0'),
    0x565300: ('CUnitObject_WaitBlocked', 'sets flags [unit+0x1ec], waits 17-51 ticks'),
    0x5267a0: ('Rules_ParseObjectTypeKey', 'switch on rules.txt key (table 0x528644)'),
    0x524ee0: ('ObjectType_Construct', 'unit/building type defaults'),
    0x4413f0: ('CUnitObject_GetDestination', 'vtable +0x1b4: returns unit+0xfc'),
    0x597bb0: ('Sides_GetSide', 'thiscall on [0x817c00] (index)'),
    0x59b350: ('World_FindObject', 'thiscall on [0x817c00] (int* id); id = index | generation<<16'),
    0x4ef5d5: ('Script_Dispatch', 'jump on script function id (table 0x4f3fb4)'),
    0x5b0c81: ('operator_new', 'CRT'),
    0x5b25e0: ('strcmp_like', 'CRT string compare (0 = equal)'),
    0x5b3148: ('vsprintf', 'CRT'),
}

# Known virtual slots of the unit classes (all unit vtables share these entries).
UNIT_VTABLE_SLOTS = {0x20: 'ReceiveOrder', 0x104: 'GetSpeed', 0x1b4: 'GetDestination', 0x29c: 'Update'}
UNIT_VTABLES = [0x5d4f18] + [0x5d607c + i * 0x2a8 for i in range(12)]


def main():
    exe, outdir = sys.argv[1], sys.argv[2]
    with_strings = '--with-strings' in sys.argv[3:]
    os.makedirs(outdir, exist_ok=True)
    pe = pefile.PE(exe)
    img = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    t0, t1 = BASE + text.VirtualAddress, BASE + text.VirtualAddress + text.Misc_VirtualSize
    rdata = next(s for s in pe.sections if s.Name.startswith(b'.rdata'))
    data = next(s for s in pe.sections if s.Name.startswith(b'.data'))
    d0, d1 = BASE + rdata.VirtualAddress, BASE + data.VirtualAddress + data.Misc_VirtualSize

    def u32(va): return struct.unpack_from('<I', img, va - BASE)[0]

    def cstr(va, n=120):
        if not (d0 <= va < d1): return None
        raw = img[va - BASE:va - BASE + n].split(b'\0')[0]
        if len(raw) < 4 or any(c < 0x20 and c not in (9, 10, 13) or c > 0x7e for c in raw): return None
        return raw.decode('latin1')

    # --- linear sweep (skips undecodable bytes, e.g. jump tables between functions) -------------------
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    insns = [(i.address, i.mnemonic, i.op_str, i.size) for i in md.disasm(bytes(img[t0 - BASE:t1 - BASE]), t0)]

    # --- function starts: direct call targets, unit vtable entries, manual names ----------------------
    starts = set()
    for a, mn, op, _ in insns:
        if mn == 'call' and op.startswith('0x'):
            v = int(op, 16)
            if t0 <= v < t1: starts.add(v)
    for vt in UNIT_VTABLES:
        for slot in range(0, 0x2a8, 4):
            v = u32(vt + slot)
            if t0 <= v < t1: starts.add(v)
    starts |= {a for a in MANUAL if t0 <= a < t1}
    starts = sorted(starts)

    def func_of(va):
        i = bisect.bisect_right(starts, va) - 1
        return starts[i] if i >= 0 else None

    names = {}   # addr -> (kind, name, source, note)

    def add(addr, kind, name, source, note=''):
        if addr in names and names[addr][2] in ('manual',): return
        names[addr] = (kind, name, source, note)

    # --- log strings and source paths ----------------------------------------------------------------
    method = re.compile(r'([A-Z][A-Za-z0-9_]{2,}::[A-Za-z_~][A-Za-z0-9_]*)')
    ident = re.compile(r'([A-Z][A-Za-z0-9_]{3,})\s*[:,(]')
    srcpath = re.compile(r'([A-Za-z]:\\MyProjects\\.*?\.(?:cpp|c|h))', re.I)
    found = collections.defaultdict(list)
    for a, mn, op, _ in insns:
        for m in re.finditer(r'0x([0-9a-f]{6})\b', op):
            s = cstr(int(m.group(1), 16), 160)
            if s:
                f = func_of(a)
                if f: found[f].append(s)
    module = {}  # function -> source file (relative to MyProjects)
    for f, strs in found.items():
        for st in strs:
            m = srcpath.search(st)
            if m:
                module[f] = m.group(1).split('MyProjects\\', 1)[-1] if 'MyProjects\\' in m.group(1) else m.group(1)
                break
    # fill: a function between two functions of the same file belongs to it (at most 40 functions apart)
    with_mod = [i for i, a in enumerate(starts) if a in module]
    for i0, i1 in zip(with_mod, with_mod[1:]):
        m0, m1 = module[starts[i0]], module[starts[i1]]
        if m0 == m1 and i1 - i0 <= 40:
            for i in range(i0 + 1, i1):
                module.setdefault(starts[i], m0)
    used = collections.Counter()
    def unique(n):
        n = re.sub(r'[^A-Za-z0-9_:~]', '_', n)
        used[n] += 1
        return n if used[n] == 1 else f'{n}_{used[n]}'
    for f, strs in found.items():
        best = None
        for st in strs:
            m = method.search(st[:90])
            if m: best = m.group(1); break
        if not best:
            for st in strs:
                if srcpath.search(st): continue
                m = ident.search(st[:70])
                if m and not m.group(1).isupper(): best = m.group(1); break
        if best:
            note = next((st for st in strs if not srcpath.search(st)), strs[0])
            note = note[:80].replace('\t', ' ').replace('\n', ' ') if with_strings else 'named after its log message'
            add(f, 'func', unique(best), 'logstr', note)

    # --- script commands -------------------------------------------------------------------------------
    for i in range(400):
        rec = 0x60a978 + i * 92
        nm = img[rec - BASE:rec - BASE + 36].split(b'\0')[0]
        if not nm or not re.fullmatch(rb'[A-Za-z_][A-Za-z0-9_]*', nm): break
        fid = u32(rec + 36)
        if fid <= 0xb1:
            h = u32(0x4f3fb4 + fid * 4)
            if t0 <= h < t1:
                add(h, 'label', 'Script_' + nm.decode(), 'script', f'script function id {fid}')

    # --- rules.txt keys of object types (unit/building) -> handler labels and type struct ----------------
    struct_rows = []
    for k in range(334):
        key_va = 0x61bff4 + k * 0xc8
        key = img[key_va - BASE:key_va - BASE + 40].split(b'\0')[0].decode('latin1')
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', key or '-'): continue
        idx = k - 0x30   # handler index for key k (lea eax,[ecx-0x2f] with ecx = k + 1)
        if not (0 <= idx <= 0x108): continue
        h = u32(0x528644 + idx * 4)
        if not (t0 <= h < t1) or h == 0x5275b0: continue
        add(h, 'label', 'Rules_' + key, 'rules', f'rules.txt key {key}')
        off, typ = None, None
        for ins in md.disasm(bytes(img[h - BASE:h - BASE + 40]), h):
            m = re.search(r'\[ebx \+ (0x[0-9a-f]+)\]', ins.op_str)
            if m and ins.mnemonic in ('fstp', 'mov'):
                off = int(m.group(1), 16)
                typ = 'float' if ins.mnemonic == 'fstp' else ('byte' if 'byte' in ins.op_str else ('word' if 'word ptr' in ins.op_str and 'dword' not in ins.op_str else 'dword'))
                break
            if ins.mnemonic in ('ret', 'jmp'): break
        if off is not None:
            struct_rows.append((off, typ, key))

    # --- unit virtual functions ------------------------------------------------------------------------
    for slot, nm in UNIT_VTABLE_SLOTS.items():
        impls = sorted({u32(vt + slot) for vt in UNIT_VTABLES})
        for j, f in enumerate(impls):
            if f not in MANUAL:
                add(f, 'func', f'CUnitObject_{nm}' + (f'_{j}' if len(impls) > 1 else ''), 'vtable', f'unit vtable +{slot:#x}')

    # --- classes via vtables: every "mov [reg], imm" with imm pointing to >= 3 code pointers in .rdata ----
    vtables = {}  # vtable -> constructor functions storing it
    for a, mn, op, _ in insns:
        m = re.match(r'dword ptr \[e[a-z]{2}(?: \+ 0x[0-9a-f]+)?\], (0x[0-9a-f]+)$', op) if mn == 'mov' else None
        if not m: continue
        v = int(m.group(1), 16)
        if not (BASE + rdata.VirtualAddress <= v < BASE + rdata.VirtualAddress + rdata.Misc_VirtualSize): continue
        if all(t0 <= u32(v + 4 * i) < t1 for i in range(3)):
            vtables.setdefault(v, set()).add(func_of(a))
    vt_sorted = sorted(vtables)
    def entries(v):
        out, i = [], 0
        while True:
            e = v + 4 * i
            if (i and e in vtables) or not (t0 <= u32(e) < t1) or i > 400: return out
            out.append(u32(e)); i += 1
    share = collections.Counter(f for v in vt_sorted for f in set(entries(v)))
    def stem(m): return re.sub(r'\.(cpp|c|h)$', '', m.split('\\')[-1], flags=re.I)
    classes = {}
    for v in vt_sorted:
        votes = collections.Counter()
        for f in entries(v):
            if f in names and '::' in names[f][1] and names[f][2] in ('logstr', 'manual'):
                votes[names[f][1].split('::')[0]] += 3 if share[f] == 1 else 1
            if share[f] == 1 and f in module:  # an override only this class has, in a known source file
                votes[stem(module[f])] += 2
        for c in vtables[v]:
            if c in module:
                votes[stem(module[c])] += 1
        if votes:
            classes[v] = votes.most_common(1)[0][0]
    for v in vt_sorted:
        cls = classes.get(v)
        if cls:
            add(v, 'vtable', f'vtbl_{cls}_{v:x}', 'vtable', f'{len(entries(v))} entries')
        for i, f in enumerate(entries(v)):
            if f in names: continue
            if share[f] == 1 and cls:
                add(f, 'func', f'{cls}::vf_{i * 4:x}', 'vtable', f'vtbl {v:#x} slot {i * 4:#x}')
            elif share[f] > 1:
                owners = sorted({classes[w] for w in vt_sorted if w in classes and f in entries(w)})
                base = 'CUnitObject' if owners and all(o == 'CUnitObject' or o.startswith('unit') for o in owners) else 'Base'
                add(f, 'func', f'{base}::vf_{i * 4:x}_{f:x}', 'vtable', f'shared by {share[f]} vtables, slot {i * 4:#x}')
        for c in vtables[v]:
            if c not in names and cls:
                add(c, 'func', f'{cls}::ctor_{c:x}', 'vtable', f'stores vtbl {v:#x}')

    for f in starts:
        if f not in names and f in module:
            stem = re.sub(r'\.(cpp|c|h)$', '', module[f].split('\\')[-1], flags=re.I)
            add(f, 'func', f'{stem}_{f:x}', 'module', '')
    for a, (n, note) in MANUAL.items():
        names[a] = ('func' if a in starts else 'label', n, 'manual', note)

    # --- write ----------------------------------------------------------------------------------------
    with open(os.path.join(outdir, 'game109-symbols.tsv'), 'w', newline='\n') as f:
        f.write('# Game.exe 1.09 (MD5 7dc4fda2f6b7e8a6b70e846686a81938) symbol map, generated by tools/re/symbols.py\n')
        f.write('# address\tkind\tname\tsource\tmodule\tnote\n')
        for a in sorted(names):
            k, n, src, note = names[a]
            mod = module.get(a if a in starts else func_of(a), '')
            f.write(f'{a:#010x}\t{k}\t{n}\t{src}\t{mod}\t{note}\n')
    with open(os.path.join(outdir, 'game109.r2'), 'w', newline='\n') as f:
        f.write('# radare2: r2 -i docs/re/game109.r2 Game.exe   (generated by tools/re/symbols.py)\n')
        for a in sorted(names):
            k, n, src, note = names[a]
            safe = re.sub(r'[^A-Za-z0-9_.]', '_', n.replace('::', '.'))
            f.write(f'f {"fcn" if k == "func" else "loc"}.{safe} @ {a:#x}\n')
    mods = collections.defaultdict(list)
    for a, m in module.items():
        mods[m].append(a)
    with open(os.path.join(outdir, 'game109-modules.tsv'), 'w', newline='\n') as f:
        f.write('# source file of the game (developer path below MyProjects)\tfirst\tlast\tfunctions\n')
        for m, addrs in sorted(mods.items(), key=lambda kv: min(kv[1])):
            f.write(f'{m}\t{min(addrs):#010x}\t{max(addrs):#010x}\t{len(addrs)}\n')
    with open(os.path.join(outdir, 'game109-unittype.tsv'), 'w', newline='\n') as f:
        f.write('# Unit/building type struct (rules.txt keys), generated by tools/re/symbols.py\n# offset\ttype\trules key\n')
        for off, typ, key in sorted(set(struct_rows)):
            f.write(f'{off:#06x}\t{typ}\t{key}\n')
    named_funcs = sum(1 for a, v in names.items() if v[0] == 'func' and v[2] != 'module')
    classes_found = len(set(classes.values()))
    module_funcs = sum(1 for a in starts if a in module)
    by_src = collections.Counter(v[2] for v in names.values())
    code_bytes = t1 - t0
    starts_arr = starts + [t1]
    named_bytes = sum(starts_arr[i + 1] - s for i, s in enumerate(starts) if s in names and names[s][0] == 'func' and names[s][2] != 'module')
    module_bytes = sum(starts_arr[i + 1] - s for i, s in enumerate(starts) if s in module)
    with open(os.path.join(outdir, 'coverage.txt'), 'w', newline='\n') as f:
        f.write(f'functions found: {len(starts)}\n')
        f.write(f'functions with an own name: {named_funcs} ({100.0 * named_funcs / len(starts):.1f} %)\n')
        f.write(f'functions with a known source file: {module_funcs} ({100.0 * module_funcs / len(starts):.1f} %) in {len(mods)} files\n')
        f.write(f'code bytes in functions with an own name: {100.0 * named_bytes / code_bytes:.1f} % of {code_bytes}\n')
        f.write(f'code bytes with a known source file: {100.0 * module_bytes / code_bytes:.1f} %\n')
        f.write(f'labels (script/rules handlers inside functions): {sum(1 for v in names.values() if v[0] == "label")}\n')
        f.write(f'unit type fields: {len(set(struct_rows))}\n')
        f.write(f'vtables found: {len(vtables)}, with a class name: {len(classes)} ({classes_found} classes)\n')
        f.write('by source: ' + ', '.join(f'{k} {v}' for k, v in sorted(by_src.items())) + '\n')
    print(open(os.path.join(outdir, 'coverage.txt')).read())


if __name__ == '__main__':
    main()
