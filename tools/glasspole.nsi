; Glasspole — Windows installer.
;
; Built from Linux by tools/build-windows.sh, which passes VERSION, STAGE and
; OUTFILE. Deliberately plain: per-user install so it needs no administrator,
; an uninstaller, Start-menu and desktop shortcuts, and nothing else. No
; bundled runtime, no registry beyond what Add/Remove Programs needs, no
; services.
;
; PER-USER, NOT PER-MACHINE, and that is a real decision: Glasspole writes its
; firmware, games and saves under %LOCALAPPDATA%, it updates itself by running
; a new installer, and neither wants a UAC prompt. A machine-wide install
; would ask for elevation to do something one user asked for.

!define APP "Glasspole"
!ifndef VERSION
  !define VERSION "dev"
!endif

Name "${APP}"
OutFile "${OUTFILE}"
Unicode true
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "Software\${APP}" "InstallDir"
ShowInstDetails show
ShowUninstDetails show
BrandingText "${APP} ${VERSION}"

; SAY WHAT THIS IS BEFORE A SINGLE FILE IS WRITTEN. Someone who downloaded
; "the LeapPad emulator" is entitled to know, before installing, that the
; Windows build runs on an emulator core written from scratch here rather
; than on the qemu-user everything else uses — and that the different look is
; deliberate, not a different program. Cancel is a real answer, so this is
; MB_OKCANCEL and not a notice they can only agree with.
Function .onInit
  MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
"This is a port of the Tadpole LeapPad 2 emulator.$\n$\n\
It runs on a highly experimental, custom-built replacement for qemu-user, \
codenamed Glasspole.$\n$\n\
Because of that, the Windows port uses different branding and colours from \
the Linux version, so you can tell at a glance which backend you are running.$\n$\n\
Continue with the installation?" \
    IDOK continue
  Abort
continue:
FunctionEnd

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "$INSTDIR"
  ; The staged tree, whole: the layout matters because every part of the
  ; program finds the others relative to tadpole.sh at the root.
  File /r "${STAGE}\*.*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortCut "$DESKTOP\${APP}.lnk" "$INSTDIR\tadpole.exe" "" "$INSTDIR\tadpole.exe" 0
  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortCut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\tadpole.exe" "" "$INSTDIR\tadpole.exe" 0
  ; The system menu, for people who want the LeapPad rather than the front end.
  CreateShortCut "$SMPROGRAMS\${APP}\${APP} (system menu).lnk" \
                 "$INSTDIR\tadpole.exe" "--boot" "$INSTDIR\tadpole.exe" 0
  CreateShortCut "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\${APP}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" \
                   "DisplayName" "${APP}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" \
                   "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" \
                   "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" \
                   "DisplayIcon" "$\"$INSTDIR\tadpole.exe$\""
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}" \
                   "NoModify" "1"
SectionEnd

Section "Uninstall"
  ; THE PROGRAM ONLY. Firmware, games and saves live in %LOCALAPPDATA%\Tadpole
  ; and are the user's — some of it took an hour to download, and none of it
  ; is ours to delete because they removed the player.
  Delete "$DESKTOP\${APP}.lnk"
  RMDir /r "$SMPROGRAMS\${APP}"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
  DeleteRegKey HKCU "Software\${APP}"
SectionEnd
