# PlatformIO pre-script: put ccache in front of the compile commands (CI build job, cloud
# sessions). Enabled only when $XINK_CCACHE names the ccache binary; set it with
# PLATFORMIO_EXTRA_SCRIPTS=pre:<abs path>/scripts/pio_ccache.py.
# (Pre, so the change is in place before library/source environments are cloned from env;
# $CC itself is expanded later, when commands run.)
# CC/CXX stay untouched because ESP-IDF steps build paths from them ($TOOLCHAIN/bin/$CC);
# only the C/C++ compile command lines get the ccache prefix.
import os

Import("env")  # noqa: F821  (SCons builtin)

cc = os.environ.get("XINK_CCACHE")
if cc:
    for var in ("CCCOM", "CXXCOM", "SHCCCOM", "SHCXXCOM"):
        cur = env.get(var)  # noqa: F821
        if cur and "ccache" not in str(cur):
            env.Replace(**{var: f'"{cc}" {cur}'})  # noqa: F821
