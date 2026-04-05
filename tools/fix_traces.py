#!/usr/bin/env python3
"""Fix trace insertions in civ_dump_lifted.c"""

FUNCS = ['far_15D8_3ACA', 'far_14F4_0004', 'far_1436_0000', 'far_1A1A_0000']

def main():
    path = 'RecompiledFuncs/civ_dump_lifted.c'
    with open(path, 'r') as f:
        content = f.read()

    # Remove any existing broken trace lines
    lines = content.split('\n')
    cleaned = []
    skip = False
    for line in lines:
        if skip:
            skip = False
            continue
        if any(f'[TRACE] {func}' in line for func in FUNCS):
            skip = True
            continue
        cleaned.append(line)

    # Re-insert proper traces
    result = []
    i = 0
    while i < len(cleaned):
        result.append(cleaned[i])
        for func in FUNCS:
            if cleaned[i].strip() == f'void {func}(CPU *cpu)':
                if i + 1 < len(cleaned) and cleaned[i + 1].strip() == '{':
                    result.append(cleaned[i + 1])  # the '{'
                    trace = '    { static int _c = 0; _c++; if (_c <= 3 || (_c %% 100000) == 0) { fprintf(stderr, "[TRACE] %s #%%d sp=%%04X\\n", _c, cpu->sp); fflush(stderr); } }' % func
                    result.append(trace)
                    i += 1  # skip the '{' since we already added it
                break
        i += 1

    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(result))

    print('Done')

if __name__ == '__main__':
    main()
