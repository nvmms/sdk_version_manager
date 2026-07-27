Unicode true
ManifestDPIAware true

!include "MUI2.nsh"

!ifndef APP_VERSION
  !define APP_VERSION "0.1.0"
!endif

!ifndef APP_ARCH
  !define APP_ARCH "x64"
!endif

!ifndef RELEASE_DIR
  !error "RELEASE_DIR must point to the staged release directory"
!endif

!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "SVM-${APP_VERSION}-windows-${APP_ARCH}-setup.exe"
!endif

!define PRODUCT_NAME "SDK Version Manager"
!define PRODUCT_PUBLISHER "SVM"
!define PRODUCT_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SDKVersionManager"

Name "${PRODUCT_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\SDK Version Manager"
InstallDirRegKey HKCU "${PRODUCT_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${PRODUCT_NAME} ${APP_VERSION} · ${APP_ARCH}"
ShowInstDetails nevershow
ShowUninstDetails nevershow

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1033 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) 2026 SVM contributors"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT_NAME} installer (${APP_ARCH})"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION}"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install ${PRODUCT_NAME}"
!define MUI_WELCOMEPAGE_TEXT "Manage Flutter, Node.js, Java, Python and more from one local-first desktop app.$\r$\n$\r$\nThis wizard will install ${PRODUCT_NAME} ${APP_VERSION} for Windows ${APP_ARCH}."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose where ${PRODUCT_NAME} should be installed. Your downloaded SDKs and local data are stored separately and will not be removed during an upgrade."
!define MUI_STARTMENUPAGE_DEFAULTFOLDER "SDK Version Manager"
!define MUI_FINISHPAGE_TITLE "${PRODUCT_NAME} is ready"
!define MUI_FINISHPAGE_TEXT "${PRODUCT_NAME} ${APP_VERSION} has been installed on your computer.$\r$\n$\r$\nClick Finish to start managing your development environments."
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\appsdk_version_manager.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch SDK Version Manager"
!define MUI_FINISHPAGE_LINK "View SDK Version Manager on GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/nvmms/sdk_version_manager"

Var StartMenuFolder

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_STARTMENU Application $StartMenuFolder
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "SDK Version Manager" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${RELEASE_DIR}\*.*"

  !insertmacro MUI_STARTMENU_WRITE_BEGIN Application
    CreateDirectory "$SMPROGRAMS\$StartMenuFolder"
    CreateShortcut "$SMPROGRAMS\$StartMenuFolder\SDK Version Manager.lnk" "$INSTDIR\bin\appsdk_version_manager.exe"
    CreateShortcut "$SMPROGRAMS\$StartMenuFolder\Uninstall SDK Version Manager.lnk" "$INSTDIR\uninstall.exe"
  !insertmacro MUI_STARTMENU_WRITE_END

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "${PRODUCT_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKCU "${PRODUCT_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${PRODUCT_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKCU "${PRODUCT_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${PRODUCT_KEY}" "DisplayIcon" "$INSTDIR\bin\appsdk_version_manager.exe"
  WriteRegStr HKCU "${PRODUCT_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${PRODUCT_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${PRODUCT_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  !insertmacro MUI_STARTMENU_GETFOLDER Application $StartMenuFolder
  Delete "$SMPROGRAMS\$StartMenuFolder\SDK Version Manager.lnk"
  Delete "$SMPROGRAMS\$StartMenuFolder\Uninstall SDK Version Manager.lnk"
  RMDir "$SMPROGRAMS\$StartMenuFolder"

  DeleteRegKey HKCU "${PRODUCT_KEY}"
  RMDir /r "$INSTDIR"
SectionEnd
