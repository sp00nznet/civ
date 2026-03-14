"""Remove stub function definitions from civ_stubs.c for functions that
are now defined in civ_dump_lifted.c."""

import re

# Read function names to remove
with open('D:/recomp/pc/civ/repo/dump_defined.txt') as f:
    to_remove = set(line.strip() for line in f if line.strip())

print(f"Functions to remove from stubs: {len(to_remove)}")

# Read stubs file
stubs_path = 'D:/recomp/pc/civ/repo/RecompiledFuncs/civ_stubs.c'
with open(stubs_path) as f:
    content = f.read()

# Remove each stub function definition
# Pattern: void func_name(CPU *cpu) { ... }
removed = 0
for name in sorted(to_remove):
    # Match the full function definition (void name(...) { ... })
    pattern = rf'\nvoid {re.escape(name)}\(CPU \*cpu\) \{{[^}}]*\}}\n'
    new_content = re.sub(pattern, '\n', content)
    if new_content != content:
        removed += 1
        content = new_content

with open(stubs_path, 'w') as f:
    f.write(content)

print(f"Removed {removed} stub definitions")
