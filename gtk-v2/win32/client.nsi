!include "MUI.nsh"

;If the user passed "-DGITVERSION=something" when invoking the script, use that. Else, use a placeholder.
!ifdef GITVERSION
!define REVISION "git-${GITVERSION}"
!else
!define REVISION "git-unknown"
!endif

;Title Of Your Application
Name "Crossfire Client"

VIAddVersionKey "ProductName" "Crossfire Client installer"
VIAddVersionKey "Comments" "Website: http://crossfire.real-time.com"
VIAddVersionKey "FileDescription" "Crossfire Client installer"
VIAddVersionKey "FileVersion" "${REVISION}"
VIAddVersionKey "LegalCopyright" "Crossfire is released under the GPL."

;If the user passed "-DVERSION=something" when invoking the script, use that. Else, use a placeholder.
;Note that it must be numerals, in the format x.x.x.x
!ifdef VERSION
VIProductVersion ${VERSION}
!else
VIProductVersion 1.99.99.99
!endif

;Do A CRC Check
CRCCheck On
SetCompressor /SOLID lzma

;Output File Name
;If the user passed "-DOUTPUTDIR=something" when invoking the script, use that. Else, use the current working directory.
!ifdef GITVERSION
!define SETUPNAME "${GITVERSION}"
!else
!define SETUPNAME "unknown"
!endif

!ifdef OUTPUTDIR
OutFile "${OUTPUTDIR}\Crossfire-GTK3-Client-${SETUPNAME}-Setup.exe"
!else
OutFile "Crossfire-GTK3-Client-${SETUPNAME}-Setup.exe"
!endif

;The Default Installation Directory
InstallDir "$PROGRAMFILES64\Crossfire Client"
InstallDirRegKey HKCU "Software\Crossfire Client" ""

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Crossfire Client (required)" cf
  SectionIn RO
  ;Install Files
  SetOutPath $INSTDIR
  SetCompress Auto
  SetOverwrite IfNewer

  ;If the user passed "-DINPUTDIR=something" when invoking the script, use that. Else, find the files in ".\files\"
  !ifdef INPUTDIR
  File /r "${INPUTDIR}\*.*"
  !else
  File /r "files\*.*"
  !endif

  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Crossfire Client" "DisplayName" "Crossfire Client"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Crossfire Client" "UninstallString" "$INSTDIR\Uninst.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Crossfire Client" "DisplayVersion" "${REVISION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Crossfire Client" "Publisher" "Crossfire"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Crossfire Client" "URLInfoAbout" "http://crossfire.real-time.com"
  WriteUninstaller "Uninst.exe"

SectionEnd

Section "Start Menu and Desktop Shortcuts" menus
  SetOutPath $INSTDIR
  CreateDirectory "$SMPROGRAMS\Crossfire"
  CreateShortCut "$SMPROGRAMS\Crossfire\Crossfire Client.lnk" \
    "$INSTDIR\crossfire-client-gtk3.exe" "" "$INSTDIR\client.ico" 0 \
    SW_SHOWNORMAL "" "Crossfire Client"
  CreateShortCut "$SMPROGRAMS\Crossfire\Uninstall.lnk" \
    "$INSTDIR\Uninst.exe" "" "$INSTDIR\Uninst.exe" 0

  SetShellVarContext all
  CreateShortcut "$DESKTOP\Crossfire Client.lnk" \
    "$INSTDIR\crossfire-client-gtk3.exe" "" "$INSTDIR\client.ico" 0 \
    SW_SHOWNORMAL "" "Crossfire Client"
SectionEnd

UninstallText "This will uninstall Crossfire Client from your system."

Section "un.Crossfire Client" un_cf
  SectionIn RO

  ;Delete Files
  RmDir /r $INSTDIR

  ;Delete Start Menu Shortcuts
  RmDir /r "$SMPROGRAMS\Crossfire"

  ;Delete Desktop Shortcut
  SetShellVarContext all
  Delete "$DESKTOP\Crossfire Client.lnk"

  ;Delete Uninstaller And Registry Entries
  Delete "$INSTDIR\Uninst.exe"
  DeleteRegKey HKEY_LOCAL_MACHINE "SOFTWARE\Crossfire Client"
  DeleteRegKey HKEY_LOCAL_MACHINE "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Crossfire Client"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${cf} "Crossfire Client application and all required runtime files."
  !insertmacro MUI_DESCRIPTION_TEXT ${menus} "Create shortcuts in Start Menu and on Desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END
