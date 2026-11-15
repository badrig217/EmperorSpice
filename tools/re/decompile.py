"""Decompiles one function of Game.exe to C-like pseudo code with angr (local analysis only - the output is
derived from the game's code and must not be committed or published).

    pip install angr
    python3 tools/re/decompile.py Game.exe 0x564d30 [size_hex=0x400]
"""
import sys
import angr

exe, addr = sys.argv[1], int(sys.argv[2], 16)
size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x400
p = angr.Project(exe, auto_load_libs=False)
cfg = p.analyses.CFGFast(regions=[(addr, addr + size)], normalize=True, function_starts=[addr])
d = p.analyses.Decompiler(cfg.kb.functions[addr], cfg=cfg.model)
print(d.codegen.text if d.codegen else 'no output')
