; Inno Setup script for Gleem for OBS on Windows.
; Built by build-installer.ps1, which passes /DAppVersion and /DStage (the
; plugin folder from `cmake --install`), plus /DSign and /Sgleem=... to sign.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\..\release\Release\obs-gleem"
#endif

[Setup]
; Never change the AppId: it is how upgrades and the uninstaller find an
; existing installation.
AppId={{48780109-F838-46A1-973B-9DB79772DC72}
AppName=Gleem for OBS
AppVersion={#AppVersion}
AppVerName=Gleem for OBS {#AppVersion}
AppPublisher=Anikeen UG (haftungsbeschränkt) & Co. KG
AppPublisherURL=https://gleem.gg
AppSupportURL=https://github.com/gleem-gg/obs-gleem
AppCopyright=© 2026 Anikeen UG (haftungsbeschränkt) & Co. KG
VersionInfoVersion={#AppVersion}
; The only per-machine plugin location OBS will keep loading, and it does not
; depend on where OBS itself is installed. See
; https://obsproject.com/kb/legacy-plugin-developer-info
DefaultDirName={commonappdata}\obs-studio\plugins\obs-gleem
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=obs-gleem-{#AppVersion}-windows-x64
UninstallDisplayName=Gleem for OBS
LicenseFile=..\..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; A running OBS holds the plugin's DLL open: Restart Manager offers to close
; it, but starting it again is left to the user.
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
#ifdef Sign
SignTool=gleem
SignedUninstaller=yes
#endif

[Messages]
FinishedLabel=Setup has finished installing [name]. Restart OBS Studio to load the plugin.

[Files]
; The stage holds the DLL twice, for OBS 33+ and for OBS 31 and 32: see
; set_target_properties_plugin in cmake/windows/helpers.cmake.
Source: "{#Stage}\*"; Excludes: "*.pdb"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
