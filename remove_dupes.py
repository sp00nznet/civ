"""Remove duplicate function definitions from civ_dump_lifted.c that are
already defined in civ_impl.c (hand-written implementations take precedence)."""

import re

# Get function names from civ_impl.c
impl_funcs = set()
with open('D:/recomp/pc/civ/repo/RecompiledFuncs/civ_impl.c') as f:
    for line in f:
        m = re.match(r'^void (\w+)\(CPU', line)
        if m and not line.strip().endswith(';'):
            impl_funcs.add(m.group(1))

# Also check stubs
stub_funcs = set()
with open('D:/recomp/pc/civ/repo/RecompiledFuncs/civ_stubs.c') as f:
    for line in f:
        m = re.match(r'^void (\w+)\(CPU', line)
        if m and not line.strip().endswith(';'):
            stub_funcs.add(m.group(1))

# Also check civ_recomp_*.c files
import os, glob
recomp_funcs = set()
for path in glob.glob('D:/recomp/pc/civ/repo/RecompiledFuncs/civ_recomp_*.c'):
    with open(path) as f:
        for line in f:
            m = re.match(r'^void (\w+)\(CPU', line)
            if m and not line.strip().endswith(';'):
                recomp_funcs.add(m.group(1))

# Read lifted file
lifted_path = 'D:/recomp/pc/civ/repo/RecompiledFuncs/civ_dump_lifted.c'
with open(lifted_path) as f:
    content = f.read()

# Get functions defined in lifted file
lifted_funcs = set()
for m in re.finditer(r'^void (\w+)\(CPU \*cpu\)\n\{', content, re.MULTILINE):
    lifted_funcs.add(m.group(1))

existing = (impl_funcs | recomp_funcs) & lifted_funcs
print(f"Impl functions: {len(impl_funcs)}")
print(f"Stub functions: {len(stub_funcs)}")
print(f"Recomp functions: {len(recomp_funcs)}")
print(f"Lifted functions: {len(lifted_funcs)}")
print(f"Duplicates to remove: {len(existing)}")

# Remove duplicate definitions
removed = 0
for name in sorted(existing):
    # Match full function definition
    pattern = rf'\n/\* Function: {re.escape(name)}.*?\n\*/\nvoid {re.escape(name)}\(CPU \*cpu\)\n\{{.*?\n\}}\n'
    new_content = re.sub(pattern, '\n', content, flags=re.DOTALL)
    if new_content != content:
        removed += 1
        content = new_content
        print(f"  Removed: {name}")
    else:
        # Try simpler pattern without comment
        pattern2 = rf'\nvoid {re.escape(name)}\(CPU \*cpu\)\n\{{.*?\n\}}\n'
        new_content = re.sub(pattern2, '\n', content, flags=re.DOTALL)
        if new_content != content:
            removed += 1
            content = new_content
            print(f"  Removed (no comment): {name}")

with open(lifted_path, 'w') as f:
    f.write(content)

print(f"\nRemoved {removed} duplicate definitions from civ_dump_lifted.c")
