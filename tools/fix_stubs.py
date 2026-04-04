#!/usr/bin/env python3
"""Fix broken stubs in civ_stubs.c by removing and re-adding them with proper escaping."""

import sys

SYMS = ['res_001219', 'res_001262', 'res_001298', 'res_00130A']

def main():
    path = 'RecompiledFuncs/civ_stubs.c'
    with open(path, 'r') as f:
        lines = f.readlines()

    # Remove lines belonging to the broken stubs
    clean = []
    skip = False
    for line in lines:
        if any('void ' + s in line for s in SYMS):
            skip = True
            continue
        if skip:
            if line.strip() == '}':
                skip = False
            continue
        clean.append(line)

    # Strip trailing blank lines
    while clean and clean[-1].strip() == '':
        clean.pop()
    clean.append('\n')

    # Add correct stubs with properly escaped newline
    for sym in SYMS:
        stub = (
            '\nvoid %s(CPU *cpu) {\n'
            '    (void)cpu;\n'
            '    static int _count = 0; _count++;\n'
            '    if (_count == 1 || (_count %% 10000) == 0)'
            ' fprintf(stderr, "[STUB] %s called (n=%%d)\\n", _count);\n'
            '    cpu->sp += 2; /* near ret */\n'
            '}\n'
        ) % (sym, sym)
        clean.append(stub)

    with open(path, 'w', newline='\n') as f:
        f.writelines(clean)
    print('Fixed stubs')

if __name__ == '__main__':
    main()
