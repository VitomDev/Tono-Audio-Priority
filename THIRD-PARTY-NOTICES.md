# Third-party notices

Tono Audio Priority contains no third-party code. It only uses Windows' own APIs (Core Audio, the shell, the registry, BCrypt and URL Monikers).

## HeadsetControl (optional, not included)

Some wireless USB headsets keep looking "connected" to Windows while they are switched off. For those, Tono can use HeadsetControl, but only if you ask for it in the test of that headset.

- It is **not** bundled with Tono, nor linked into it.
- When you ask for it, Tono downloads version 4.2.0 (`headsetcontrol-windows-x86_64.exe`) from its official release on GitHub, checks its SHA-256 hash (`81acae4c083aaa40a2e9c1bce7b3dd9625e74b9d9ee96384bb43b2dff7ff0fa5`) and keeps it in `%LOCALAPPDATA%\Tono Audio Priority`.
- Tono runs it as a separate program (`headsetcontrol -b -o json`) and only reads its output.
- Uninstalling Tono removes it.

Source: https://github.com/Sapd/HeadsetControl
Copyright: Denis Arnst (Sapd) and contributors.
License: GNU General Public License v3.0 (https://www.gnu.org/licenses/gpl-3.0.html).
