# Copy to local-paths.ps1 (untracked) and adjust.
$env:VCPKG_ROOT        = 'C:\path\to\vcpkg'
$env:KRI_COMMONLIB_DIR = 'C:\path\to\CommonLibSSE-NG-9.1.0'
# Optional: MO2 mod folder that tools\build.ps1 deploys to (leave empty to only build).
$env:KRI_MO2_DEV_MOD   = 'C:\path\to\ModOrganizer\mods\KroosRecycleItems-dev'
