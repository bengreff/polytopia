# Replaces Ghidra's func_0xADDR call targets with il2cpp method names from symbols.tsv.
import re, sys
base = 0x100000
names = {}
for line in open(sys.argv[1]):
    off, _, name = line.rstrip("\n").split("\t")
    names.setdefault(int(off, 16) + base, name)
def sub(m):
    a = int(m.group(1), 16)
    return names.get(a, m.group(0))
sys.stdout.write(re.sub(r"func_0x([0-9a-f]+)", sub, sys.stdin.read()))
