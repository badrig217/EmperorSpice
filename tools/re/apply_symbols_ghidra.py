# Ghidra script (Python/Jython or PyGhidra): applies docs/re/game109-symbols.tsv to an open Game.exe 1.09.
# Script Manager -> run, then choose the TSV. Names functions (creating them if needed), sets labels and
# plate comments with source and module. Only our own names/notes - see tools/re/symbols.py.
#@category EmperorSpice
from ghidra.program.model.symbol import SourceType

tsv = askFile("Emperor Spice symbol map", "Apply").getAbsolutePath()
fm = currentProgram.getFunctionManager()
st = currentProgram.getSymbolTable()
listing = currentProgram.getListing()
count = 0
for line in open(tsv):
    if line.startswith('#') or not line.strip():
        continue
    parts = line.rstrip('\n').split('\t')
    addr = toAddr(int(parts[0], 16))
    kind, name, source = parts[1], parts[2].replace('::', '_'), parts[3]
    module = parts[4] if len(parts) > 4 else ''
    note = parts[5] if len(parts) > 5 else ''
    if kind == 'func':
        f = fm.getFunctionAt(addr) or createFunction(addr, None)
        if f is not None:
            f.setName(name, SourceType.USER_DEFINED)
    else:
        st.createLabel(addr, name, SourceType.USER_DEFINED)
    text = '[%s] %s %s' % (source, module, note)
    cu = listing.getCodeUnitAt(addr)
    if cu is not None:
        cu.setComment(cu.PLATE_COMMENT, text.strip())
    count += 1
print('Emperor Spice: applied %d symbols' % count)
