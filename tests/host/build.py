import re, sys, os
sk = sys.argv[1]
files = [os.path.join(sk, 'UsbRs485Dongle.ino')] + sorted(os.path.join(sk,f) for f in os.listdir(sk) if f.endswith('.ino') and f != 'UsbRs485Dongle.ino')
lines = []
for f in files:
    pass
    lines += open(f).read().split('\n')
rx = re.compile(r'^(?!static\b)(?!return\b)([A-Za-z_][\w:<>]*[\s\*&]+)+(\w+)\s*\(([^;{}]*)\)\s*\{')
protos, first = [], None
inraw=False
for i, l in enumerate(lines):
    if 'R"rawliteral(' in l: inraw = True
    if inraw:
        if ')rawliteral"' in l: inraw = False
        continue
    m = rx.match(l)
    if m and m.group(2) not in ('if','for','while','switch'):
        if first is None: first = i
        protos.append(l[:l.rindex(')')+1] + ';')
lines[first:first] = protos
open('sketch.cpp','w').write('\n'.join(lines))
print(len(protos), 'prototypes')
