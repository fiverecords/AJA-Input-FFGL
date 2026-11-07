# Releasing

How a release of AJA Input is built and published on GitHub.

## Never change after a release

Compositions store the plugin by its ID and its parameter values by
index, and users install the DLL by name. Once a version is out:

- the FFGL plugin ID stays `AJIN` and the DLL `AJAInput.dll`;
- parameters keep their index: new ones go at the end of `ParamID`
  (src/AJAInputPlugin.h);
- options keep their values: new Input entries take the values after the
  last one (src/InputId.h), new choices of an option go at the end.

The unit tests check the Input option values; the Linux host smoke test
(`tests/host`, built with `-DAJAINPUT_BUILD_HOST_SMOKE=ON`) checks the
ID and the parameter order.

## Steps

1. **Version.** Set it in two places: `VERSION` in `CMakeLists.txt` and
   `kPluginVersionMajor` / `kPluginVersionMinor` in
   `src/AJAInputPlugin.cpp` (what Resolume shows). Move the changes into
   a new section of `CHANGELOG.md`, and write the release notes in
   `docs/release-notes/v<version>.md`.
2. **Build on Windows** from a clean tree:
   `build.cmd -Clean` (builds Release and runs the unit tests).
3. **Check in Resolume** with `build.cmd -Install` (Resolume closed):
   the plugin loads as AJA Input with the new version, each input you
   can test shows a picture, a cable pull shows No Signal and recovers.
4. **Package** the DLL with the notices its licences require, from the
   repository folder in PowerShell (the ZIP goes next to the folder, not
   into the repository):

   ```powershell
   $v = '0.43.0-beta'
   Compress-Archive -Force -DestinationPath "..\AJAInput-$v-win64.zip" `
       -Path build\Release\AJAInput.dll, LICENSE, THIRD_PARTY_NOTICES.md, README.md
   ```

   (With `-Generator Ninja` the DLL is in `build\` instead.)
5. **Tag and push:** commit, then `git tag v0.43.0-beta` and
   `git push origin main v0.43.0-beta`.
6. **GitHub release:** *Releases > Draft a new release*, pick the tag,
   title `AJA Input 0.43.0 beta`, paste the release notes, attach the
   ZIP, and tick *Set as a pre-release* for betas. GitHub adds the
   source code archives of the tag to the release by itself: that is the
   source offer the LGPL asks for alongside the binary.

## First publication

- Create the repository `AJA-Input-FFGL` (public, no template files:
  the licence and README are already here), then from the project
  folder: `git init`, `git add .`, `git commit -m "AJA Input 0.43.0 beta"`,
  `git branch -M main`, `git remote add origin
  https://github.com/fiverecords/AJA-Input-FFGL.git`, `git push -u origin
  main`, and continue with step 5.
- The `.gitattributes` keeps `build.cmd` and `build.ps1` with Windows
  line endings; the Linux CI (`.github/workflows/ci.yml`) runs on every
  push.
- Under *Settings > General > Features*, keep Issues on: the issue
  template asks for what a hardware report needs.
