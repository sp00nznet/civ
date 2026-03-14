import sys

syms = open('D:/recomp/pc/civ/repo/missing_from_lifted.txt').read().strip().split('\n')
lines = []
for sym in syms:
    if sym.startswith('far_'):
        ret = 'cpu->sp += 4; /* far ret */'
    else:
        ret = 'cpu->sp += 2; /* near ret */'
    lines.append(
        'void %s(CPU *cpu) {\n'
        '    (void)cpu;\n'
        '    static int _count = 0; _count++;\n'
        '    if (_count == 1 || (_count %% 10000) == 0) fprintf(stderr, "[STUB] %s called (n=%%d)\\n", _count);\n'
        '    %s\n'
        '}\n' % (sym, sym, ret))
with open('D:/recomp/pc/civ/repo/new_stubs.txt', 'w') as f:
    f.write('\n'.join(lines))
print(f'Generated {len(syms)} stubs')
