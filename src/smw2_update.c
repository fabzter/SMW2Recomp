/* Update check on start. See smw2_update.h. Windows only; other platforms get
 * no-op stubs and no launcher checkbox. */
#include "smw2_update.h"

#if defined(_WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define SMW2_UPDATE_REPO "mpolizel/SMW2Recomp"
#define SMW2_UPDATE_ASSET_SUFFIX "-windows-x64.zip"

/* The updater, run by Windows PowerShell 5.1 once the game has exited. The
 * setup pack holds only the source tree and the setup program: robocopy adds
 * and replaces files and never deletes, so build\ (the rebuilt game, saves\,
 * save states, config.ini, keybinds.ini) and rom.cfg are left as they are;
 * the /XF list guards them by name as well. Touching the setup executable
 * makes it newer than the rebuilt game, which is how the setup host knows to
 * offer Generate & rebuild once instead of starting the old build; after the
 * rebuild the game is newer again and the setup hands off to it. (Forcing the
 * wizard with SNESRECOMP_FORCE_SETUP instead would be inherited by the rebuilt
 * game and bring the wizard back after every rebuild.) The setup reads the
 * ROM path from the root rom.cfg; when that is missing or stale but the
 * game's build\rom.cfg names a ROM that exists, the root one is pointed at it
 * so the player is never asked for the ROM again.
 *
 * An update must never cost the player a save. Before copying, build\saves
 * (in-game saves and save states) is copied to save-backups\<time>_before_<v>;
 * nothing ever deletes those copies. Save files are excluded from the copy by
 * name and directory, and if build\saves differs afterwards anyway, the
 * backup is put back. */
static const char kUpdaterScript[] =
  "param([string]$Root, [string]$Url, [int]$WaitPid, [string]$Version)\r\n"
  "$ErrorActionPreference = 'Stop'\r\n"
  "$Host.UI.RawUI.WindowTitle = \"Super Mario World 2: Yoshi's Island Recomp - updating to $Version\"\r\n"
  "try {\r\n"
  "  Write-Host \"Super Mario World 2: Yoshi's Island Recomp update to $Version\"\r\n"
  "  Wait-Process -Id $WaitPid -ErrorAction SilentlyContinue\r\n"
  "  $work = Join-Path $env:TEMP \"smw2-update-$Version\"\r\n"
  "  if (Test-Path $work) { Remove-Item -Recurse -Force $work }\r\n"
  "  New-Item -ItemType Directory $work | Out-Null\r\n"
  "  $zip = Join-Path $work 'update.zip'\r\n"
  "  Write-Host \"Downloading $Url\"\r\n"
  "  & curl.exe -L --fail -o $zip $Url\r\n"
  "  if ($LASTEXITCODE -ne 0) { throw \"download failed (curl exit $LASTEXITCODE)\" }\r\n"
  "  Write-Host 'Extracting...'\r\n"
  "  Expand-Archive -Path $zip -DestinationPath (Join-Path $work 'x')\r\n"
  "  $src = Get-ChildItem (Join-Path $work 'x') -Directory | Select-Object -First 1\r\n"
  "  if (-not $src) { throw 'the update package is empty' }\r\n"
  "  $saves = Join-Path $Root 'build\\saves'\r\n"
  "  $backup = $null\r\n"
  "  $hashes = { @(Get-ChildItem $saves -Recurse -File | Sort-Object FullName | "
      "ForEach-Object { $_.FullName + '|' + (Get-FileHash $_.FullName).Hash }) -join \"`n\" }\r\n"
  "  if (Test-Path $saves) {\r\n"
  "    $backup = Join-Path $Root ('save-backups\\' + (Get-Date -Format 'yyyy-MM-dd_HHmmss') + \"_before_$Version\")\r\n"
  "    Write-Host \"Backing up your saves and save states to $backup\"\r\n"
  "    & robocopy.exe $saves $backup /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null\r\n"
  "    if ($LASTEXITCODE -ge 8) { throw \"the save backup failed (robocopy exit $LASTEXITCODE)\" }\r\n"
  "    $before = & $hashes\r\n"
  "  }\r\n"
  "  Write-Host 'Installing (saves, save states and settings are kept)...'\r\n"
  "  & robocopy.exe $src.FullName $Root /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP "
      "/XF config.ini keybinds.ini rom.cfg update.ini *.srm *.sav *.thumb *.bak *.state "
      "/XD saves save-backups | Out-Null\r\n"
  "  if ($LASTEXITCODE -ge 8) { throw \"copy failed (robocopy exit $LASTEXITCODE)\" }\r\n"
  "  if ($backup -and (& $hashes) -ne $before) {\r\n"
  "    & robocopy.exe $backup $saves /E /IS /IT /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null\r\n"
  "    Write-Host 'Your saves were changed during the copy and have been restored from the backup.' -ForegroundColor Yellow\r\n"
  "  }\r\n"
  "  $romCfg = Join-Path $Root 'rom.cfg'\r\n"
  "  $gameRomCfg = Join-Path $Root 'build\\rom.cfg'\r\n"
  "  $rootRom = if (Test-Path $romCfg) { (Get-Content $romCfg -Raw).Trim() } else { '' }\r\n"
  "  if ((-not $rootRom -or -not (Test-Path -LiteralPath $rootRom)) -and (Test-Path $gameRomCfg)) {\r\n"
  "    $gameRom = (Get-Content $gameRomCfg -Raw).Trim()\r\n"
  "    if ($gameRom -and (Test-Path -LiteralPath $gameRom)) {\r\n"
  "      [IO.File]::WriteAllText($romCfg, $gameRom)\r\n"
  "      Write-Host \"Keeping your ROM: $gameRom\"\r\n"
  "    }\r\n"
  "  }\r\n"
  "  $setup = Join-Path $Root 'SuperMarioWorld2YoshisIslandSNESRecomp.exe'\r\n"
  "  (Get-Item $setup).LastWriteTime = Get-Date\r\n"
  "  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue\r\n"
  "  Write-Host ''\r\n"
  "  Write-Host 'Done. The setup window opens next: press Generate & rebuild to build the new version.'\r\n"
  "  Start-Process -FilePath $setup -WorkingDirectory $Root\r\n"
  "  Start-Sleep -Seconds 4\r\n"
  "} catch {\r\n"
  "  Write-Host ''\r\n"
  "  Write-Host \"Update failed: $_\" -ForegroundColor Red\r\n"
  "  Write-Host 'Your game, saves and settings were not changed by the failed step.'\r\n"
  "  if ($backup) { Write-Host \"A copy of your saves is in $backup\" }\r\n"
  "  Read-Host 'Press Enter to close'\r\n"
  "}\r\n";

static int ExeDir(wchar_t *out, size_t cap) {
  DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
  if (n == 0 || n >= cap) return 0;
  wchar_t *slash = wcsrchr(out, L'\\');
  if (!slash) return 0;
  *slash = 0;
  return 1;
}

static int SettingsPath(wchar_t *out, size_t cap) {
  if (!ExeDir(out, cap)) return 0;
  return wcscat_s(out, cap, L"\\update.ini") == 0;
}

int Smw2UpdateGetEnabled(void) {
  wchar_t path[MAX_PATH];
  if (!SettingsPath(path, MAX_PATH)) return 1;
  return GetPrivateProfileIntW(L"Update", L"CheckOnStart", 1, path) != 0;
}

void Smw2UpdateSetEnabled(int enabled) {
  wchar_t path[MAX_PATH];
  if (!SettingsPath(path, MAX_PATH)) return;
  WritePrivateProfileStringW(L"Update", L"CheckOnStart", enabled ? L"1" : L"0", path);
}

static int PathExists(const wchar_t *dir, const wchar_t *name) {
  wchar_t p[MAX_PATH];
  if (swprintf(p, MAX_PATH, L"%ls\\%ls", dir, name) < 0) return 0;
  return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* The install root: the setup pack's top folder, which is the executable's
 * own folder for the setup program and its parent for the game in build\. */
static int FindInstallRoot(wchar_t *root, size_t cap) {
  if (!ExeDir(root, cap)) return 0;
  for (int up = 0; up < 3; up++) {
    if (PathExists(root, L"rom_identity.txt") && PathExists(root, L"snesrecomp"))
      return 1;
    wchar_t *slash = wcsrchr(root, L'\\');
    if (!slash) return 0;
    *slash = 0;
  }
  return 0;
}

/* Runs a command line with no window and waits up to timeout_ms. */
static int RunHidden(wchar_t *cmdline, DWORD timeout_ms) {
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi;
  DWORD code = 1;
  if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                      NULL, &si, &pi))
    return 0;
  if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT)
    TerminateProcess(pi.hProcess, 1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

/* The string value of the first "key": "..." after from, or 0. */
static int JsonString(const char *from, const char *key, char *out, size_t cap,
                      const char **after) {
  char pattern[64];
  snprintf(pattern, sizeof pattern, "\"%s\"", key);
  const char *p = strstr(from, pattern);
  if (!p) return 0;
  p = strchr(p + strlen(pattern), '"');
  if (!p) return 0;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  if (after) *after = p;
  return n > 0;
}

/* "v1.2.3" / "1.2.3" -> comparable integer; 0 when it does not parse. */
static long VersionValue(const char *s) {
  int a = 0, b = 0, c = 0;
  if (*s == 'v' || *s == 'V') s++;
  if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 2) return 0;
  return (long)a * 1000000L + (long)b * 1000L + c;
}

/* Latest release tag and its Windows setup-pack URL from the GitHub API. */
static int FetchLatest(char *tag, size_t tag_cap, char *url, size_t url_cap) {
  wchar_t tmp_dir[MAX_PATH], json_path[MAX_PATH], cmd[2048];
  if (!GetTempPathW(MAX_PATH, tmp_dir)) return 0;
  swprintf(json_path, MAX_PATH, L"%lssmw2-latest-release.json", tmp_dir);
  DeleteFileW(json_path);
  swprintf(cmd, 2048,
           L"curl.exe -s -L --fail --connect-timeout 3 --max-time 6 "
           L"-H \"Accept: application/vnd.github+json\" "
           L"-H \"User-Agent: SuperMarioWorld2YoshisIslandSNESRecomp-updater\" -o \"%ls\" "
           L"https://api.github.com/repos/" L"" SMW2_UPDATE_REPO L"/releases/latest",
           json_path);
  if (!RunHidden(cmd, 8000)) return 0;

  FILE *f = _wfopen(json_path, L"rb");
  if (!f) return 0;
  static char json[262144];
  size_t n = fread(json, 1, sizeof json - 1, f);
  fclose(f);
  DeleteFileW(json_path);
  json[n] = 0;

  if (!JsonString(json, "tag_name", tag, tag_cap, NULL)) return 0;
  const char *p = json;
  char candidate[1024];
  while (JsonString(p, "browser_download_url", candidate, sizeof candidate, &p)) {
    size_t len = strlen(candidate), sl = strlen(SMW2_UPDATE_ASSET_SUFFIX);
    if (len > sl && strcmp(candidate + len - sl, SMW2_UPDATE_ASSET_SUFFIX) == 0 &&
        strlen(candidate) < url_cap) {
      strcpy(url, candidate);
      return 1;
    }
  }
  return 0;
}

static int StartUpdater(const wchar_t *root, const char *url, const char *tag) {
  wchar_t tmp_dir[MAX_PATH], script[MAX_PATH], cmd[4096], wurl[1024], wtag[64];
  if (!GetTempPathW(MAX_PATH, tmp_dir)) return 0;
  swprintf(script, MAX_PATH, L"%lssmw2-update.ps1", tmp_dir);
  FILE *f = _wfopen(script, L"wb");
  if (!f) return 0;
  fputs(kUpdaterScript, f);
  fclose(f);
  MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
  MultiByteToWideChar(CP_UTF8, 0, tag, -1, wtag, 64);
  swprintf(cmd, 4096,
           L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%ls\" "
           L"-Root \"%ls\" -Url \"%ls\" -WaitPid %lu -Version \"%ls\"",
           script, root, wurl, (unsigned long)GetCurrentProcessId(), wtag);
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
                      root, &si, &pi))
    return 0;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 1;
}

void Smw2UpdateCheckOnStart(const char *current_version) {
  wchar_t root[MAX_PATH];
  char tag[64], url[1024];
  if (!current_version || !VersionValue(current_version)) return;  /* "dev" */
  if (!Smw2UpdateGetEnabled()) return;
  if (!FindInstallRoot(root, MAX_PATH)) return;
  if (PathExists(root, L".git")) return;  /* a checkout updates through git */
  if (!FetchLatest(tag, sizeof tag, url, sizeof url)) {
    fprintf(stderr, "[update] no release information (offline?)\n");
    return;
  }
  fprintf(stderr, "[update] installed %s, latest %s\n", current_version, tag);
  if (VersionValue(tag) <= VersionValue(current_version)) return;

  wchar_t text[1024], wtag[64], wcur[64];
  MultiByteToWideChar(CP_UTF8, 0, tag, -1, wtag, 64);
  MultiByteToWideChar(CP_UTF8, 0, current_version, -1, wcur, 64);
  swprintf(text, 1024,
           L"Super Mario World 2: Yoshi's Island Recomp %ls is available (you have %ls).\n\n"
           L"Update now? The game closes, the update is downloaded and installed, "
           L"and the setup window opens to rebuild the game.\n\n"
           L"Your in-game saves, save states and settings are kept, and a copy "
           L"of your saves is made first in the save-backups folder.\n\n"
           L"You can turn this check off on the launcher's main screen "
           L"(\"Check for updates on start\", next to \"Skip launcher on boot\").",
           wtag, wcur);
  if (MessageBoxW(NULL, text, L"Super Mario World 2: Yoshi's Island Recomp - update available",
                  MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) != IDYES)
    return;
  if (!StartUpdater(root, url, tag)) {
    MessageBoxW(NULL, L"The updater could not be started. The game will run as is.",
                L"Super Mario World 2: Yoshi's Island Recomp", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return;
  }
  fprintf(stderr, "[update] updater started for %s; exiting\n", tag);
  exit(0);
}

#else

void Smw2UpdateCheckOnStart(const char *current_version) { (void)current_version; }
int Smw2UpdateGetEnabled(void) { return 0; }
void Smw2UpdateSetEnabled(int enabled) { (void)enabled; }

#endif
