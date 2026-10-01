"""
PlatformIO pre-build script: inject git info into version defines.

  default:       1.1.0-dev+<branch>  (local development builds)
  production:    1.1.0               (when $CROSSDINK_RELEASE_VERSION is set)
  RC:            1.1.0-<hash>-RC      (when $CROSSDINK_RC_HASH is set)
  test & debug:          1.2.6-<branch>+<5-char-hash>
  gh_release_rc: 1.1.0-<hash>-RC       (hash from $CROSSDINK_RC_HASH in CI,
                                        or from git locally)

Simulator environments set CROSSDINK_VERSION directly in platformio.ini.
"""

import configparser
import datetime
import os
import re
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def get_git_short_hash(project_dir, length=5):
    try:
        return subprocess.check_output(
            ['git', 'rev-parse', '--short', 'HEAD'],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()[:length]
    except Exception as e:
        warn(f'Could not read git hash: {e}; hash will be "00000"')
        return '00000'


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return sanitize_version_component(branch)


def sanitize_version_component(value):
    value = value.strip()
    value = re.sub(r'[^A-Za-z0-9._-]+', '-', value)
    value = re.sub(r'-{2,}', '-', value)
    value = value.strip('-.')
    return value or 'unknown'


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_git_dirty(project_dir):
    try:
        status = subprocess.check_output(
            ['git', 'status', '--porcelain', '--untracked-files=no'],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        )
        return '1' if status.strip() else '0'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(f'Could not read git working-tree state: {e}; state will be "unknown"')
        return 'unknown'


def get_build_number(project_dir):
    # Batch/CI builds may pass their own number; otherwise use the commit count,
    # which only means something when the full history is checked out. Shallow
    # CI checkouts would all report 1, so they get no number at all.
    number = os.environ.get('CROSSDINK_BUILD_NUMBER')
    if number:
        return sanitize_version_component(number)
    if run_git_value(project_dir, ['rev-parse', '--is-shallow-repository'], 'shallow state') != 'false':
        return ''
    number = run_git_value(project_dir, ['rev-list', '--count', 'HEAD'], 'build number')
    return number if number.isdigit() else ''


def short_branch_label(branch):
    # Batch branches (test/combined-0927-b11) show as their batch number; other
    # branches drop their prefix folder (claude/, feature/, fix/).
    batch = re.search(r'combined-\d+-(b\d+)$', branch)
    if batch:
        return batch.group(1)
    return branch.rsplit('/', 1)[-1] or 'unknown'


def register_build_info(env, project_dir, scoped_defines):
    # The build time changes on every build. Defining it globally would change
    # every compile command and force a full rebuild, so scope these defines to
    # the one small file that exposes them.
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    if branch == 'HEAD':
        branch = 'detached'
    branch = re.sub(r'[^A-Za-z0-9._/-]+', '-', branch) or 'unknown'
    build_time = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%MZ')
    defines = [
        ('CROSSDINK_GIT_BRANCH', f'\\"{branch}\\"'),
        ('CROSSDINK_GIT_BRANCH_SHORT', f'\\"{short_branch_label(branch)}\\"'),
        ('CROSSDINK_BUILD_NUMBER', f'\\"{get_build_number(project_dir)}\\"'),
        ('CROSSDINK_BUILD_TIME', f'\\"{build_time}\\"'),
        # The commit and dirty flag change with every commit or first edit; as
        # global defines they forced a full rebuild each time.
        ('CROSSDINK_GIT_SHA', f'\\"{get_git_short_sha(project_dir)}\\"'),
        ('CROSSDINK_GIT_DIRTY', f'\\"{get_git_dirty(project_dir)}\\"'),
        # The version carries the branch and commit in test builds.
        *scoped_defines,
    ]

    def add_build_info_defines(node_env, node):
        build_env = node_env.Clone()
        build_env.Append(CPPDEFINES=defines)
        return build_env.Object(node)

    env.AddBuildMiddleware(add_build_info_defines, '*src/util/BuildInfo.cpp')


def _read_ini(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    local_ini_path = os.path.join(project_dir, 'platformio.local.ini')
    config = configparser.ConfigParser()
    if os.path.isfile(ini_path):
        config_paths = [ini_path]
        if os.path.isfile(local_ini_path):
            # Match PlatformIO's local override convention: values from the
            # optional local file take precedence over the tracked config.
            config_paths.append(local_ini_path)
        config.read(config_paths)
    else:
        warn(f'platformio.ini not found at {ini_path}')
    return config


def get_crossdink_version(project_dir):
    config = _read_ini(project_dir)
    if not config.has_option('crossdink', 'version'):
        warn(
            'No [crossdink] version in platformio.ini or platformio.local.ini; '
            'build version will be "0.0.0"'
        )
        return '0.0.0'
    return config.get('crossdink', 'version')


def get_release_candidate_version(project_dir):
    short_hash = os.environ.get('CROSSDINK_RC_HASH') or get_git_short_hash(project_dir)
    base_version = re.sub(r'-RC$', '', get_crossdink_version(project_dir), flags=re.IGNORECASE)
    return f'{base_version}-{sanitize_version_component(short_hash)}-RC'


def get_production_version(project_dir):
    release_version = os.environ.get('CROSSDINK_RELEASE_VERSION')
    if release_version:
        return sanitize_version_component(release_version.lstrip('v'))
    return get_crossdink_version(project_dir)


def get_hardware_version(project_dir, pioenv):
    if os.environ.get('CROSSDINK_RC_HASH'):
        return get_release_candidate_version(project_dir)

    if pioenv == 'default':
        if os.environ.get('CROSSDINK_RELEASE_VERSION'):
            return get_production_version(project_dir)
        base_version = get_crossdink_version(project_dir)
        branch = get_git_branch(project_dir)
        return f'{base_version}-dev+{branch}'

    base_version = (
        get_production_version(project_dir)
        if os.environ.get('CROSSDINK_RELEASE_VERSION')
        else get_crossdink_version(project_dir)
    )
    device_suffix = {
        'sticky': '-sticky',
        'x4-pro': '-x4-pro',
        'x4-classic': '-x4-classic',
    }[pioenv]
    return f'{base_version}{device_suffix}'


def inject_version(env):
    project_dir = env['PROJECT_DIR']
    pioenv = env['PIOENV']
    # Keep build provenance separate from CROSSDINK_VERSION: production versions
    # intentionally omit the source revision, while diagnostics need the base
    # commit and whether the compiled tree had tracked modifications. Those two
    # live in register_build_info() (BuildInfo.cpp only).
    env.Append(CPPDEFINES=[
        ('CROSSDINK_PIOENV', f'\\"{pioenv}\\"'),
    ])

    # CROSSDINK_VERSION goes only to src/util/BuildInfo.cpp (see register_build_info).
    scoped = []

    if pioenv in {'default', 'sticky', 'x4-pro', 'x4-classic'}:
        version_string = get_hardware_version(project_dir, pioenv)
        if os.environ.get('CROSSDINK_RC_HASH'):
            print(f'CrossDink RC build version: {version_string}')
        elif os.environ.get('CROSSDINK_RELEASE_VERSION'):
            print(f'CrossDink production build version: {version_string}')
        else:
            print(f'CrossDink build version: {version_string}')
        scoped.append(('CROSSDINK_VERSION', f'\\"{version_string}\\"'))

    elif pioenv == 'debug':
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        ci_version = get_crossdink_version(project_dir)
        suffix = f'-{branch}+{short_hash}'
        scoped.append(('CROSSDINK_VERSION', f'\\"{ci_version}{suffix}\\"'))
        env.Append(CPPDEFINES=[
            ('CROSSDINK_BUILD_ENV', '\\"debug\\"'),
            'CROSSDINK_SHOW_SLEEP_BUILD_INFO',
        ])
        print(f'CrossDink test build version: {ci_version}{suffix}')

    elif pioenv == 'sticky-debug':
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        ci_version = get_crossdink_version(project_dir)
        suffix = f'-{branch}+{short_hash}'
        scoped.append(('CROSSDINK_VERSION', f'\\"{ci_version}{suffix}\\"'))
        env.Append(CPPDEFINES=[
            ('CROSSDINK_BUILD_ENV', '\\"debug\\"'),
            'CROSSDINK_SHOW_SLEEP_BUILD_INFO',
        ])
        print(f'CrossDink test build version: {ci_version}{suffix}')

    elif pioenv in {'x4-pro-debug', 'x4-classic-debug'}:
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        ci_version = get_crossdink_version(project_dir)
        suffix = f'-{branch}+{short_hash}'
        scoped.append(('CROSSDINK_VERSION', f'\\"{ci_version}{suffix}\\"'))
        env.Append(CPPDEFINES=[
            ('CROSSDINK_BUILD_ENV', '\\"debug\\"'),
            'CROSSDINK_SHOW_SLEEP_BUILD_INFO',
        ])
        print(f'CrossDink test build version: {ci_version}{suffix}')

    elif pioenv == 'test':
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        ci_version = get_crossdink_version(project_dir)
        suffix = f'-{branch}+{short_hash}'
        scoped.append(('CROSSDINK_VERSION', f'\\"{ci_version}{suffix}\\"'))
        print(f'CrossDink test build version: {ci_version}{suffix}')

    elif pioenv == 'gh_release_rc':
        # CI passes CROSSDINK_RC_HASH as an env var; locally we derive it from git.
        version_string = get_release_candidate_version(project_dir)
        scoped.append(('CROSSDINK_VERSION', f'\\"{version_string}\\"'))
        print(f'CrossDink RC build version: {version_string}')

    if hasattr(env, 'AddBuildMiddleware'):
        register_build_info(env, project_dir, scoped)
    else:
        env.Append(CPPDEFINES=scoped)


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')  # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    if '__file__' in globals():
        _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    else:
        _project_dir = os.getcwd()
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
else:
    inject_version(env)  # noqa: F821  # type: ignore[name-defined]
