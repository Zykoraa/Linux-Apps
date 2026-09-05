# -*- mode: python ; coding: utf-8 -*-
#
# A self-contained Linux build, for people who would rather download one
# archive than run install.sh.
#
# install.sh is still the better way to install this. A PyInstaller build
# carries its own copy of every shared library it can see, including the
# libc it was built against, so it runs on that distribution and newer and
# not on older -- and its ffmpeg, if one is bundled, never gets a security
# update. The venv install has neither problem.
#
#   pip install pyinstaller
#   python -m PyInstaller gui.spec --noconfirm
#
# Output lands in dist/evesgarden/. Keep the binary and _internal/ together.

import os

datas = [('assets', 'assets')]
# ffmpeg is expected on PATH -- see ffmpeg_tools. bin/ is only bundled when
# somebody has deliberately put binaries there for a fully offline archive.
if os.path.isdir('bin'):
    datas.append(('bin', 'bin'))

a = Analysis(
    ['gui.py'],
    pathex=[],
    binaries=[],
    datas=datas,
    # Imported lazily or through a plugin lookup, so PyInstaller's static
    # analysis cannot see them:
    #   pypresence   inside DiscordPresence._connect()
    #   dbus_fast    the MPRIS service, started only if there is a session bus
    #   Xlib         window-manager cooperation, imported inside functions
    #   audioop      pydub's PEP 594 backport, imported by name
    hiddenimports=['PIL.ImageTk', 'pypresence', 'dbus_fast',
                   'dbus_fast.aio', 'dbus_fast.service',
                   'Xlib', 'Xlib.display', 'Xlib.protocol.event', 'audioop'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    # Named for the app rather than 'gui': this is what shows in ps, in the
    # taskbar's fallback, and in the crash of anyone who packages it.
    name='evesgarden',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    # upx is off. It saves little on a Linux ELF and breaks some loaders,
    # and a compressed binary is what several antivirus heuristics look for.
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='evesgarden',
)
