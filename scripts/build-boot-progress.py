#!/usr/bin/env python3
"""Build the opt-in board diagnostic against this source kernel's actual ABI.

The sources are the MIT boot-progress component of the sheng port (package/BootProgress.c and
friends); this board only changes where they are found and which UEFI checkout supplies Font.h.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
sources = ROOT / 'kits/tb128fu'
font_dir = Path(os.environ.get('UNIXNU_UEFI') or ROOT / 'uefi-tb128fu') / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
source = sources / 'BootProgress.c'
obj = OUT / 'BootProgress.o'
command = []
args = record['arguments']
i = 0
while i < len(args):
    arg = args[i]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        i += 1
    elif arg == '-Xclang' and args[i+1].startswith('-W'):
        i += 1
    elif arg == '-D' and args[i+1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    i += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(font_dir)]
subprocess.run(command, cwd=record['directory'], check=True, timeout=90)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
fault_source = sources / 'BootFaultProbe.c'
fault_obj = OUT / 'BootFaultProbe.o'
fault_command = [str(fault_source) if arg == str(source) else str(fault_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(fault_command, cwd=record['directory'], check=True, timeout=90)
stub_source = sources / 'BootProgressStubs.c'
stub_obj = OUT / 'BootProgressStubs.o'
stub_command = [str(stub_source) if arg == str(source) else str(stub_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(stub_command, cwd=record['directory'], check=True, timeout=90)
# The UFS diagnostic log writer (sheng-log=1), unchanged from the sheng port; package/linux-log reads the window back.
log_source = sources / 'BootLog.c'
log_obj = OUT / 'BootLog.o'
log_command = [str(log_source) if arg == str(source) else str(log_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(log_command, cwd=record['directory'], check=True, timeout=90)
# Its reboot-into-fastboot backend (sheng-autoreset=<seconds>): the PM6125 PON reboot reason over SPMI, then PSCI SYSTEM_RESET.
reset_source = sources / 'BootReset.c'
reset_obj = OUT / 'BootReset.o'
reset_command = [str(reset_source) if arg == str(source) else str(reset_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(reset_command, cwd=record['directory'], check=True, timeout=90)
# The CPU frequency domains for the log (read only unless tb128fu-cpufreq=1).
cpufreq_source = sources / 'CpuFreq.c'
cpufreq_obj = OUT / 'CpuFreq.o'
cpufreq_command = [str(cpufreq_source) if arg == str(source) else str(cpufreq_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(cpufreq_command, cwd=record['directory'], check=True, timeout=90)
# sheng's process pass and UI thread sampler, unchanged: the stacks of SpringBoard and the other traced processes (and, with
# boot-ui-follow=1, the wait chain behind SpringBoard) for the page and the log.
probe_objs = []
for probe in ('BootProcessProbe', 'BootUiTraceProbe'):
    probe_source = sources / f'{probe}.c'
    probe_obj = OUT / f'{probe}.o'
    subprocess.run([str(probe_source) if arg == str(source) else str(probe_obj) if arg == str(obj) else arg for arg in command],
                   cwd=record['directory'], check=True, timeout=90)
    probe_objs.append(probe_obj)
policy_source = sources / 'BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
policy_command = []
policy_args = policy_record['arguments']
j = 0
while j < len(policy_args):
    arg = policy_args[j]
    if arg == policy_record['file']:
        policy_command.append(str(policy_source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            policy_command += ['-o', str(policy_obj)]
        j += 1
    elif arg == '-Xclang' and policy_args[j+1].startswith('-W'):
        j += 1
    elif not arg.startswith('-W'):
        policy_command.append(arg)
    j += 1
policy_command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
subprocess.run(policy_command, cwd=policy_record['directory'], check=True, timeout=90)

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(fault_obj), str(policy_obj), str(stub_obj), str(log_obj),
                str(reset_obj), str(cpufreq_obj), *map(str, probe_objs)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'watchdog_policy_sha256': hashlib.sha256(policy_source.read_bytes()).hexdigest(),
    'fault_sha256': hashlib.sha256(fault_source.read_bytes()).hexdigest(),
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'stub_sha256': hashlib.sha256(stub_source.read_bytes()).hexdigest(),
    'log_sha256': hashlib.sha256(log_source.read_bytes()).hexdigest(),
    'reset_sha256': hashlib.sha256(reset_source.read_bytes()).hexdigest(),
    'cpufreq_sha256': hashlib.sha256(cpufreq_source.read_bytes()).hexdigest(),
    'lifecycle_sha256': hashlib.sha256((source.parent / 'BootProgressLifecycle.h').read_bytes()).hexdigest(),
    'core_sha256': hashlib.sha256((source.parent / 'BootProgressCore.h').read_bytes()).hexdigest(),
    'display_sha256': hashlib.sha256((source.parent / 'BootProgressDisplay.h').read_bytes()).hexdigest(),
    'pulse_sha256': hashlib.sha256((source.parent / 'BootProgressPulse.h').read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
}, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 boot diagnostic build')
