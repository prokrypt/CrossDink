"""
PlatformIO post-build script: fail when firmware.bin does not fit in the
smallest app partition from partitions.csv, or when the data in RTC slow
memory grows past RTC_SLOW_BUDGET.
"""

import csv
import os
import re
import subprocess
import sys

# ESP32-S3 RTC slow memory (data bus) and the budget for everything linked there
# (.rtc_noinit, .rtc.data, .rtc.bss, .rtc.force_slow). The linker allows 7,680 B
# (8 KB less the 512 B ULP reserve); the budget keeps headroom so a new RTC record
# is a deliberate bump here, not a silent grab of the last bytes.
RTC_SLOW_START = 0x50000000
RTC_SLOW_END = 0x50002000
RTC_SLOW_BUDGET = 7168


def _parse_size(value):
    value = value.strip()
    match = re.fullmatch(r'(\d+)([KkMm])?', value)
    if match:
        size = int(match.group(1), 10)
        suffix = match.group(2)
        if suffix in ('K', 'k'):
            return size * 1024
        if suffix in ('M', 'm'):
            return size * 1024 * 1024
        return size
    return int(value, 0)


def _get_project_option(env, name):
    try:
        value = env.GetProjectOption(name)
    except Exception:
        return None
    if isinstance(value, str):
        value = value.strip()
    return value or None


def _get_partition_file(project_dir, env):
    partition_file = _get_project_option(env, 'board_build.partitions')
    if not partition_file:
        return None
    if os.path.isabs(partition_file):
        return partition_file
    return os.path.join(project_dir, partition_file)


def _get_app_partition_limit(partition_file):
    app_sizes = []
    app_labels = []

    with open(partition_file, newline='', encoding='utf-8') as fp:
        for row in csv.reader(fp):
            if not row:
                continue

            row = [cell.strip() for cell in row]
            if not row[0] or row[0].startswith('#') or len(row) < 5:
                continue

            name, partition_type, _subtype, _offset, size = row[:5]
            if partition_type != 'app':
                continue

            app_labels.append(name)
            app_sizes.append(_parse_size(size))

    if not app_sizes:
        return None, None

    return min(app_sizes), ', '.join(app_labels)


def check_firmware_size(source, target, env):
    del source

    firmware_path = str(target[0])
    firmware_size = os.path.getsize(firmware_path)
    project_dir = env['PROJECT_DIR']
    partition_file = _get_partition_file(project_dir, env)

    if not partition_file or not os.path.exists(partition_file):
        print('Unable to find partition table for firmware size check', file=sys.stderr)
        env.Exit(1)

    limit, labels = _get_app_partition_limit(partition_file)
    if not limit:
        print(f'No app partition found in {partition_file}', file=sys.stderr)
        env.Exit(1)

    remaining = limit - firmware_size
    rel_partition_file = os.path.relpath(partition_file, project_dir)
    if remaining < 0:
        print(
            f'Firmware image is too large for OTA app partition: '
            f'{firmware_size} bytes > {limit} bytes '
            f'({rel_partition_file}: {labels}); over by {-remaining} bytes',
            file=sys.stderr,
        )
        env.Exit(1)

    print(
        f'Firmware image fits OTA app partition: '
        f'{firmware_size} bytes <= {limit} bytes '
        f'({remaining} bytes free)',
    )

    check_rtc_slow(firmware_path[:-len('.bin')] + '.elf', env)


def check_rtc_slow(elf_path, env):
    size_tool = env.subst('$SIZETOOL')
    try:
        out = subprocess.run([size_tool, '-A', elf_path], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as err:
        print(f'RTC slow check: cannot run {size_tool!r} on {elf_path}: {err}', file=sys.stderr)
        env.Exit(1)

    sections = []
    for line in out.splitlines():
        parts = line.split()  # "name size addr", decimal
        if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
            name, size, addr = parts[0], int(parts[1]), int(parts[2])
            if size and RTC_SLOW_START <= addr < RTC_SLOW_END:
                sections.append((name, size))
    used = sum(size for _, size in sections)
    detail = ', '.join(f'{name} {size}' for name, size in sections) or 'none'
    if used > RTC_SLOW_BUDGET:
        print(
            f'RTC slow memory over budget: {used} bytes > {RTC_SLOW_BUDGET} ({detail}); '
            f'shrink a record or raise RTC_SLOW_BUDGET in scripts/check_firmware_size.py on purpose',
            file=sys.stderr,
        )
        env.Exit(1)
    print(f'RTC slow memory: {used} bytes <= {RTC_SLOW_BUDGET} budget ({detail})')


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        check_firmware_size,
    )
except NameError:
    print('check_firmware_size.py: must be run via PlatformIO', file=sys.stderr)
