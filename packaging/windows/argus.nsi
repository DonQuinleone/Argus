; NSIS installer script for Argus (audio master QA).
; Driven by packaging/windows/build-windows.ps1, which stages the built files and
; invokes: makensis /DVERSION=x.y.z /DSTAGE=<dir> /DOUTFILE=<setup.exe> argus.nsi
;
; The build is unsigned by default; users may see a SmartScreen "unknown
; publisher" prompt. To sign, run signtool on Argus.exe / the produced setup.exe
; with an Authenticode certificate before/after makensis.

!define APPNAME "Argus"
!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef STAGE
  !define STAGE "stage"
!endif
!ifndef OUTFILE
  !define OUTFILE "Argus-${VERSION}-setup.exe"
!endif

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
Unicode true
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "$INSTDIR"
  File "${STAGE}\Argus.exe"
  File "${STAGE}\argus.exe"
  ; Bundle runtime DLLs staged next to the exe (libsndfile + codecs, MSVC runtime).
  File /nonfatal "${STAGE}\*.dll"

  SetOutPath "$INSTDIR\resources"
  File "${STAGE}\resources\JetBrainsMono-Regular.ttf"
  File "${STAGE}\resources\JetBrainsMono-Bold.ttf"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\Argus.exe"
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\Argus.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  !define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "Argus"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\Argus.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\Argus.exe"
  Delete "$INSTDIR\argus.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\resources\JetBrainsMono-Regular.ttf"
  Delete "$INSTDIR\resources\JetBrainsMono-Bold.ttf"
  RMDir "$INSTDIR\resources"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
  DeleteRegKey HKLM "Software\${APPNAME}"
SectionEnd
