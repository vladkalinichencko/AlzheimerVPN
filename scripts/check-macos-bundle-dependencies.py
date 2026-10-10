from pathlib import Path
import subprocess
import sys

bundle = Path(sys.argv[1]) / 'Contents'
missing = []
count = 0
for binary in bundle.rglob('*'):
    if not binary.is_file() or binary.is_symlink():
        continue
    with binary.open('rb') as stream:
        if stream.read(4) not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xce\xfa\xed\xfe'):
            continue
    count += 1
    install_names = subprocess.check_output(['otool', '-D', str(binary)], text=True).splitlines()[1:]
    output = subprocess.check_output(['otool', '-L', str(binary)], text=True)
    for line in output.splitlines()[1:]:
        dependency = line.strip().split(' (compatibility')[0]
        if dependency in install_names:
            continue
        if dependency.startswith(('/System/', '/usr/lib/')):
            continue
        if dependency.startswith('@rpath/'):
            target = bundle / 'Frameworks' / dependency.removeprefix('@rpath/')
        elif dependency.startswith('@executable_path/'):
            target = bundle / 'MacOS' / dependency.removeprefix('@executable_path/')
        elif dependency.startswith('@loader_path/'):
            target = binary.parent / dependency.removeprefix('@loader_path/')
        else:
            missing.append((binary, dependency, 'external path'))
            continue
        if not target.exists():
            missing.append((binary, dependency, 'missing file'))
for binary, dependency, reason in missing:
    print(f'{reason}: {binary.relative_to(bundle)} -> {dependency}')
print(f'Checked {count} Mach-O files; {len(missing)} unresolved or external dependencies')
sys.exit(bool(missing))
