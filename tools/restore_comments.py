"""Restore inherited comments damaged by the bulk rename of a forked ns-3 module.
Only comment text is changed (// ... and lines of /* */ blocks); code and strings are untouched."""
import re, sys

def fix_comment(txt, fam):
    # attribution of the original AODV implementations
    txt = re.sub(r'NS-2 (?:SA)?Q?(?:SQ|L)?M?AODV model', 'NS-2 AODV model', txt)
    txt = re.sub(r'(?:SA)?Q?(?:SQ|L)?M?AODV-UU', 'AODV-UU', txt)
    if fam == 'qsqmaodv':
        txt = re.sub(r'(?<![-\w])QSQMAODV(?![-\w])', 'QMAODV', txt)
    elif fam == 'qlaodv':
        txt = re.sub(r'(?<![-\w])QLAODV(?![-\w])', 'AODV', txt)
    return txt

def transform(src, fam):
    out, in_block = [], False
    for line in src.split('\n'):
        s = line.lstrip()
        if in_block or s.startswith('/*') or s.startswith('*'):
            out.append(fix_comment(line, fam))
            if '*/' in line: in_block = False
            elif s.startswith('/*'): in_block = True
            continue
        # trailing // comment (ignore '//' inside string literals: crude check on quote parity)
        i = line.find('//')
        if i >= 0 and line[:i].count('"') % 2 == 0:
            out.append(line[:i] + fix_comment(line[i:], fam))
        else:
            out.append(line)
    return '\n'.join(out)

if __name__ == '__main__':
    fam = sys.argv[1]
    if fam == 'none':
        sys.exit(0)
    for p in sys.argv[2:]:
        t = open(p, encoding='utf-8').read()
        n = transform(t, fam)
        if n != t: open(p, 'w', encoding='utf-8').write(n)
