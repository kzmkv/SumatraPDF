/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef BUILD_UNINSTALLER
#error "BUILD_UNINSTALLER must be defined!!!"
#endif

#include "Installer.h"
#define UNINSTALLER_WIN_DX  INSTALLER_WIN_DX
#define UNINSTALLER_WIN_DY  INSTALLER_WIN_DY

// Try harder getting temporary directory
// Caller needs to free() the result.
// Returns NULL if fails for any reason.
static TCHAR *GetValidTempDir()
{
    TCHAR d[MAX_PATH];
    DWORD res = GetTempPath(dimof(d), d);
    if ((0 == res) || (res >= MAX_PATH)) {
        NotifyFailed(_T("Couldn't obtain temporary directory"));
        return NULL;
    }
    BOOL success = CreateDirectory(d, NULL);
    if (!success && (ERROR_ALREADY_EXISTS != GetLastError())) {
        LogLastError();
        NotifyFailed(_T("Couldn't create temporary directory"));
        return NULL;
    }
    return str::Dup(d);
}

static TCHAR *GetTempUninstallerPath()
{
    ScopedMem<TCHAR> tempDir(GetValidTempDir());
    if (!tempDir)
        return NULL;
    // Using fixed (unlikely) name instead of GetTempFileName()
    // so that we don't litter temp dir with copies of ourselves
    return path::Join(tempDir, _T("sum~inst.exe"));
}

BOOL IsUninstallerNeeded()
{
    ScopedMem<TCHAR> exePath(GetInstalledExePath());
    return file::Exists(exePath);
}

static bool RemoveUninstallerRegistryInfo(HKEY hkey)
{
    bool ok1 = DeleteRegKey(hkey, REG_PATH_UNINST);
    // this key was added by installers up to version 1.8
    bool ok2 = DeleteRegKey(hkey, REG_PATH_SOFTWARE);
    return ok1 && ok2;
}

/* Undo what DoAssociateExeWithPdfExtension() in AppTools.cpp did */
static void UnregisterFromBeingDefaultViewer(HKEY hkey)
{
    ScopedMem<TCHAR> curr(ReadRegStr(hkey, REG_CLASSES_PDF, NULL));
    ScopedMem<TCHAR> prev(ReadRegStr(hkey, REG_CLASSES_APP, _T("previous.pdf")));
    if (!curr || !str::Eq(curr, TAPP)) {
        // not the default, do nothing
    } else if (prev) {
        WriteRegStr(hkey, REG_CLASSES_PDF, NULL, prev);
    } else {
        SHDeleteValue(hkey, REG_CLASSES_PDF, NULL);
    }

    // the following settings overrule HKEY_CLASSES_ROOT\.pdf
    ScopedMem<TCHAR> buf(ReadRegStr(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT, PROG_ID));
    if (str::Eq(buf, TAPP)) {
        LONG res = SHDeleteValue(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT, PROG_ID);
        if (res != ERROR_SUCCESS)
            LogLastError(res);
    }
    buf.Set(ReadRegStr(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT, APPLICATION));
    if (str::EqI(buf, EXENAME)) {
        LONG res = SHDeleteValue(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT, APPLICATION);
        if (res != ERROR_SUCCESS)
            LogLastError(res);
    }
    buf.Set(ReadRegStr(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT _T("\\UserChoice"), PROG_ID));
    if (str::Eq(buf, TAPP))
        DeleteRegKey(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT _T("\\UserChoice"), true);
}

static bool DeleteEmptyRegKey(HKEY root, const TCHAR *keyName)
{
    HKEY hkey;
    if (RegOpenKeyEx(root, keyName, 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return true;

    DWORD subkeys, values;
    bool isEmpty = false;
    if (RegQueryInfoKey(hkey, NULL, NULL, NULL, &subkeys, NULL, NULL,
                        &values, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        isEmpty = 0 == subkeys && 0 == values;
    }
    RegCloseKey(hkey);

    if (isEmpty)
        DeleteRegKey(root, keyName);
    return isEmpty;
}

static void RemoveOwnRegistryKeys()
{
    HKEY keys[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    // remove all keys from both HKLM and HKCU (wherever they exist)
    for (int i = 0; i < dimof(keys); i++) {
        UnregisterFromBeingDefaultViewer(keys[i]);
        DeleteRegKey(keys[i], REG_CLASSES_APP);
        DeleteRegKey(keys[i], REG_CLASSES_APPS);
        SHDeleteValue(keys[i], REG_CLASSES_PDF _T("\\OpenWithProgids"), TAPP);

        for (int j = 0; NULL != gSupportedExts[j]; j++) {
            ScopedMem<TCHAR> keyname(str::Join(_T("Software\\Classes\\"), gSupportedExts[j], _T("\\OpenWithProgids")));
            SHDeleteValue(keys[i], keyname, TAPP);
            DeleteEmptyRegKey(keys[i], keyname);

            keyname.Set(str::Join(_T("Software\\Classes\\"), gSupportedExts[j], _T("\\OpenWithList\\") EXENAME));
            if (!DeleteRegKey(keys[i], keyname))
                continue;
            // remove empty keys that the installer might have created
            *(TCHAR *)str::FindCharLast(keyname, '\\') = '\0';
            if (!DeleteEmptyRegKey(keys[i], keyname))
                continue;
            *(TCHAR *)str::FindCharLast(keyname, '\\') = '\0';
            DeleteEmptyRegKey(keys[i], keyname);
        }
    }
}

static BOOL RemoveEmptyDirectory(TCHAR *dir)
{
    WIN32_FIND_DATA findData;
    BOOL success = TRUE;

    ScopedMem<TCHAR> dirPattern(path::Join(dir, _T("*")));
    HANDLE h = FindFirstFile(dirPattern, &findData);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            ScopedMem<TCHAR> path(path::Join(dir, findData.cFileName));
            DWORD attrs = findData.dwFileAttributes;
            // filter out directories. Even though there shouldn't be any
            // subdirectories, it also filters out the standard "." and ".."
            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) &&
                !str::Eq(findData.cFileName, _T(".")) &&
                !str::Eq(findData.cFileName, _T(".."))) {
                success &= RemoveEmptyDirectory(path);
            }
        } while (FindNextFile(h, &findData) != 0);
        FindClose(h);
    }

    if (!RemoveDirectory(dir)) {
        DWORD lastError = GetLastError();
        if (ERROR_DIR_NOT_EMPTY != lastError && ERROR_FILE_NOT_FOUND != lastError) {
            LogLastError(lastError);
            success = FALSE;
        }
    }

    return success;
}

static BOOL RemoveInstalledFiles()
{
    BOOL success = TRUE;

    for (int i = 0; NULL != gPayloadData[i].filepath; i++) {
        ScopedMem<TCHAR> relPath(str::conv::FromUtf8(gPayloadData[i].filepath));
        ScopedMem<TCHAR> path(path::Join(gGlobalData.installDir, relPath));

        if (file::Exists(path))
            success &= DeleteFile(path);
    }

    RemoveEmptyDirectory(gGlobalData.installDir);
    return success;
}


// If this is uninstaller and we're running from installation directory,
// copy uninstaller to temp directory and execute from there, exiting
// ourselves. This is needed so that uninstaller can delete itself
// from installation directory and remove installation directory
// If returns TRUE, this is an installer and we sublaunched ourselves,
// so the caller needs to exit
bool ExecuteUninstallerFromTempDir()
{
    // only need to sublaunch if running from installation dir
    ScopedMem<TCHAR> ownDir(path::GetDir(GetOwnPath()));
    ScopedMem<TCHAR> tempPath(GetTempUninstallerPath());

    // no temp directory available?
    if (!tempPath)
        return false;

    // not running from the installation directory?
    // (likely a test uninstaller that shouldn't be removed anyway)
    if (!path::IsSame(ownDir, gGlobalData.installDir))
        return false;

    // already running from temp directory?
    if (path::IsSame(GetOwnPath(), tempPath))
        return false;

    if (!CopyFile(GetOwnPath(), tempPath, FALSE)) {
        NotifyFailed(_T("Failed to copy uninstaller to temp directory"));
        return false;
    }

    ScopedMem<TCHAR> args(str::Format(_T("/d \"%s\" %s"), gGlobalData.installDir, gGlobalData.silent ? _T("/s") : _T("")));
    bool ok = CreateProcessHelper(tempPath, args);

    // mark the uninstaller for removal at shutdown (note: works only for administrators)
    MoveFileEx(tempPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    return ok;
}

static bool RemoveShortcut(bool allUsers)
{
    ScopedMem<TCHAR> p(GetShortcutPath(allUsers));
    if (!p.Get())
        return false;
    bool ok = DeleteFile(p);
    if (!ok && (ERROR_FILE_NOT_FOUND != GetLastError())) {
        LogLastError();
        return false;
    }
    return true;
}

DWORD WINAPI UninstallerThread(LPVOID data)
{
    // also kill the original uninstaller, if it's just spawned
    // a DELETE_ON_CLOSE copy from the temp directory
    TCHAR *exePath = GetUninstallerPath();
    if (!path::IsSame(exePath, GetOwnPath()))
        KillProcess(exePath, TRUE);
    free(exePath);

    if (!RemoveUninstallerRegistryInfo(HKEY_LOCAL_MACHINE) &&
        !RemoveUninstallerRegistryInfo(HKEY_CURRENT_USER)) {
        NotifyFailed(_T("Failed to delete uninstaller registry keys"));
    }

    if (!RemoveShortcut(true) && !RemoveShortcut(false))
        NotifyFailed(_T("Couldn't remove the shortcut"));

    UninstallBrowserPlugin();
    UninstallPdfFilter();
    UninstallPdfPreviewer();
    RemoveOwnRegistryKeys();

    if (!RemoveInstalledFiles())
        NotifyFailed(_T("Couldn't remove installation directory"));

    // always succeed, even for partial uninstallations
    gGlobalData.success = true;

    if (!gGlobalData.silent)
        PostMessage(gHwndFrame, WM_APP_INSTALLATION_FINISHED, 0, 0);
    return 0;
}

static void OnButtonUninstall()
{
    KillSumatra();

    if (!CheckInstallUninstallPossible())
        return;

    // disable the button during uninstallation
    EnableWindow(gHwndButtonInstUninst, FALSE);
    SetMsg(_T("Uninstallation in progress..."), COLOR_MSG_INSTALLATION);
    InvalidateFrame();

    gGlobalData.hThread = CreateThread(NULL, 0, UninstallerThread, NULL, 0, 0);
}

void OnUninstallationFinished()
{
    DestroyWindow(gHwndButtonInstUninst);
    gHwndButtonInstUninst = NULL;
    CreateButtonExit(gHwndFrame);
    SetMsg(TAPP _T(" has been uninstalled."), gMsgError ? COLOR_MSG_FAILED : COLOR_MSG_OK);
    gMsgError = gGlobalData.firstError;
    InvalidateFrame();

    CloseHandle(gGlobalData.hThread);
}

bool OnWmCommand(WPARAM wParam)
{
    switch (LOWORD(wParam))
    {
        case IDOK:
            if (gHwndButtonInstUninst)
                OnButtonUninstall();
            else if (gHwndButtonExit)
                OnButtonExit();
            break;

        case ID_BUTTON_EXIT:
        case IDCANCEL:
            OnButtonExit();
            break;

        default:
            return false;
    }
    return true;
}

void OnCreateWindow(HWND hwnd)
{
    gHwndButtonInstUninst = CreateDefaultButton(hwnd, _T("Uninstall ") TAPP, 150);
}

void CreateMainWindow()
{
    gHwndFrame = CreateWindow(
        INSTALLER_FRAME_CLASS_NAME, TAPP _T(" ") CURR_VERSION_STR _T(" Uninstaller"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        dpiAdjust(INSTALLER_WIN_DX), dpiAdjust(INSTALLER_WIN_DY),
        NULL, NULL,
        ghinst, NULL);
}

void ShowUsage()
{
    MessageBox(NULL, _T("uninstall.exe [/s][/d <path>]\n\
    \n\
    /s\tuninstalls ") TAPP _T(" silently (without user interaction).\n\
    /d\tchanges the directory from where ") TAPP _T(" will be uninstalled."), TAPP _T(" Uninstaller Usage"), MB_OK | MB_ICONINFORMATION);
}
