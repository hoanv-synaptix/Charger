# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    # Bundle assets/ (brand icons) into the exe -- without this,
    # _apply_brand_icon()'s iconphoto() call can't find the PNGs at
    # runtime once frozen (main.py's __file__-relative path resolution
    # doesn't point into the PyInstaller extraction dir), so the running
    # window/taskbar icon silently falls back to nothing even though the
    # .exe file itself has the icon= below.
    datas=[('assets', 'assets')],
    hiddenimports=[],
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
    a.binaries,
    a.datas,
    [],
    name='ChargerDebugApp',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['assets/pkg_icon.ico'],
)
