Unicode true

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

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1033 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT_NAME} installer (${APP_ARCH})"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION}"

Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

Section "SDK Version Manager" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${RELEASE_DIR}\*.*"

  CreateDirectory "$SMPROGRAMS\SDK Version Manager"
  CreateShortcut "$SMPROGRAMS\SDK Version Manager\SDK Version Manager.lnk" "$INSTDIR\bin\appsdk_version_manager.exe"

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
  Delete "$SMPROGRAMS\SDK Version Manager\SDK Version Manager.lnk"
  RMDir "$SMPROGRAMS\SDK Version Manager"

  DeleteRegKey HKCU "${PRODUCT_KEY}"
  RMDir /r "$INSTDIR"
SectionEnd
