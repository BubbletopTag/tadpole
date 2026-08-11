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

; NO DEFAULT VERSION. This used to quietly define "dev" when nothing passed
; -DVERSION, which meant a missing or empty variable in the release script
; produced a perfectly ordinary-looking Glasspole-Setup.exe whose branding and
; whose Add/Remove Programs entry both said "dev" — and there is no later step
; that could have caught it. The only caller is tools/build-windows.sh and it
; always passes one, so anything else is a build bug and should stop here
; rather than ship.
!ifndef VERSION
  !error "VERSION was not defined — build this through tools/build-windows.sh"
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
;
; NOT ON AN UPDATE, THOUGH. Tadpole runs this installer with /S when it is
; updating itself, and someone who is already running the program has both
; seen this notice and answered it. Asking again — and then asking for an
; install directory, and then for a click on Install — is the whole of why
; updating on Windows felt like a chore next to Linux, where the AppImage
; swaps itself and reopens with no questions at all.
Function .onInit
  IfSilent done
  MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
"This is a port of the Tadpole LeapPad 2 emulator.$\n$\n\
It runs on a highly experimental, custom-built replacement for qemu-user, \
codenamed Glasspole.$\n$\n\
Because of that, the Windows port uses different branding and colours from \
the Linux version, so you can tell at a glance which backend you are running.$\n$\n\
Continue with the installation?" \
    IDOK done
  Abort
done:
FunctionEnd

; AND PUT THE PROGRAM BACK WHEN AN UPDATE FINISHES.
;
; This is the last piece of the asymmetry the update path had. On Linux the
; viewer renames the new AppImage over the running one and execv()s it, so the
; user watches the window blink and come back on the new version. On Windows it
; downloaded an installer, launched it, exited — and stopped there, leaving
; someone who had asked for an update looking at a closed program and an
; installer wizard, with the last step ("open it again") left to them.
;
; Only on the silent path: someone who ran the installer by hand from Explorer
; did not ask for the program to start, and an installer that launches things
; unbidden is its own kind of rude. $INSTDIR is right in both cases because
; InstallDirRegKey reads back the directory the previous install recorded.
; Labels rather than `IfSilent 0 +2`: a relative jump that is one instruction
; out fails in the direction that launches the program when nobody asked, and
; no compiler will tell you.
Function .onInstSuccess
  IfSilent 0 no_relaunch
    Exec '"$INSTDIR\tadpole.exe"'
no_relaunch:
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

  ; NAMED FOR THE PRODUCT, PICTURED FOR THE BACKEND. What someone installed is
  ; the Tadpole emulator, so that is what the icon says; the Glasspole frog is
  ; what it looks like, which is the same signal the window and its chrome
  ; give. The icon comes from tadpole.exe's own resource, so shortcut,
  ; task bar and Add/Remove Programs all show one artwork.
  CreateShortCut "$DESKTOP\Tadpole Emulator.lnk" "$INSTDIR\tadpole.exe" "" "$INSTDIR\tadpole.exe" 0
  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortCut "$SMPROGRAMS\${APP}\Tadpole Emulator.lnk" "$INSTDIR\tadpole.exe" "" "$INSTDIR\tadpole.exe" 0
  ; The system menu, for people who want the LeapPad rather than the front end.
  CreateShortCut "$SMPROGRAMS\${APP}\Tadpole Emulator (system menu).lnk" \
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
  Delete "$DESKTOP\Tadpole Emulator.lnk"
  Delete "$DESKTOP\${APP}.lnk"        ; a shortcut left by a build before the rename
  RMDir /r "$SMPROGRAMS\${APP}"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
  DeleteRegKey HKCU "Software\${APP}"
SectionEnd
