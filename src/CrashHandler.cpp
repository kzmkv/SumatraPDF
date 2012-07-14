/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD */

#include "BaseUtil.h"
#include <dbghelp.h>
#include <tlhelp32.h>
#include "AppTools.h"
#include "CrashHandler.h"
#include "DbgHelpDyn.h"
#include "FileUtil.h"
#include "Http.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "Version.h"
#include "WinUtil.h"
#include "ZipUtil.h"
#include <unzalloc.h>

#define NOLOG 0 // 0 for more detailed debugging, 1 to disable lf()
#include "DebugLog.h"

#ifndef SYMBOL_DOWNLOAD_URL
#ifdef SVN_PRE_RELEASE_VER
#define SYMBOL_DOWNLOAD_URL _T("http://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-") _T(QM(SVN_PRE_RELEASE_VER)) _T(".pdb.zip")
#else
#define SYMBOL_DOWNLOAD_URL _T("http://kjkpub.s3.amazonaws.com/sumatrapdf/rel/SumatraPDF-") _T(QM(CURR_VERSION)) _T(".pdb.zip")
#endif
#endif

#if !defined(CRASH_SUBMIT_SERVER) || !defined(CRASH_SUBMIT_URL)
#define CRASH_SUBMIT_SERVER _T("blog.kowalczyk.info")
#define CRASH_SUBMIT_URL    _T("/app/crashsubmit?appname=SumatraPDF")
#endif

// The following functions allow crash handler to be used by both installer
// and sumatra proper. They must be implemented for each app.
extern void GetStressTestInfo(str::Str<char>* s);
extern bool CrashHandlerCanUseNet();
extern void CrashHandlerMessage();
extern void GetProgramInfo(str::Str<char>& s);

/* Note: we cannot use standard malloc()/free()/new()/delete() in crash handler.
For multi-thread safety, there is a per-heap lock taken insid HeapAlloc() etc.
It's possible that a crash originates from  inside such functions after a lock
has been taken. If we then try to allocate memory from the same heap, we'll
deadlock and won't send crash report.
For that reason we create a heap used only for crash handler and must only
allocate, directly or indirectly, from that heap.
I'm not sure what happens if a Windows function (e.g. http calls) has to
allocate memory. I assume it'll use GetProcessHeap() heap and further assume
that CRT creates its own heap for malloc()/free() etc. so that while a deadlock
is still possible, the probability should be greatly reduced. */

class CrashHandlerAllocator : public Allocator {
    HANDLE allocHeap;

public:
    CrashHandlerAllocator() {
        allocHeap = HeapCreate(0, 128 * 1024, 0);
    }
    virtual ~CrashHandlerAllocator() {
        HeapDestroy(allocHeap);
    }
    virtual void *Alloc(size_t size) {
        return HeapAlloc(allocHeap, 0, size);
    }
    virtual void *Realloc(void *mem, size_t size) {
        return HeapReAlloc(allocHeap, 0, mem, size);
    }
    virtual void Free(void *mem) {
        HeapFree(allocHeap, 0, mem);
    }
};

enum ExeType {
    // this is an installer, SumatraPDF-${ver}-install.exe
    ExeInstaller,
    // this is a single-executable (portable) build (doesn't have libmupdf.dll)
    ExeSumatraStatic,
    // an installable build (has libmupdf.dll)
    ExeSumatraLib
};

static CrashHandlerAllocator *gCrashHandlerAllocator = NULL;

// Note: intentionally not using ScopedMem<> to avoid
// static initializers/destructors, which are bad
static TCHAR *  gCrashDumpPath = NULL;
static WCHAR *  gSymbolPathW = NULL;
static TCHAR *  gSymbolsDir = NULL;
static TCHAR *  gPdbZipPath = NULL;
static TCHAR *  gLibMupdfPdbPath = NULL;
static TCHAR *  gSumatraPdfPdbPath = NULL;
static TCHAR *  gInstallerPdbPath = NULL;
static char *   gSystemInfo = NULL;
static char *   gModulesInfo = NULL;
static str::Str<char> *gAdditionalInfo = NULL;
static HANDLE   gDumpEvent = NULL;
static HANDLE   gDumpThread = NULL;
static ExeType  gExeType = ExeSumatraStatic;

static MINIDUMP_EXCEPTION_INFORMATION gMei = { 0 };
static LPTOP_LEVEL_EXCEPTION_FILTER gPrevExceptionFilter = NULL;

// alloc/free functions to be passed to unzip.c
static void *zip_alloc(void *opaque, size_t items, size_t size)
{
    void *v = gCrashHandlerAllocator->Alloc(items * size);
    return v;
}

static void zip_free(void *opaque, void *addr)
{
    gCrashHandlerAllocator->Free(addr);
}

static UnzipAllocFuncs gUnzipAllocFuncs = {
    &zip_alloc,
    &zip_free
};

static char *BuildCrashInfoText()
{
    lf("BuildCrashInfoText(): start");

    str::Str<char> s(16 * 1024, gCrashHandlerAllocator);
    if (gSystemInfo)
        s.Append(gSystemInfo);

    GetStressTestInfo(&s);
    s.Append("\r\n");

    dbghelp::GetExceptionInfo(s, gMei.ExceptionPointers);
    dbghelp::GetAllThreadsCallstacks(s);
    s.Append("\r\n");
#if 0 // disabled because crashes in release builds
    s.AppendFmt("Thread: %x\r\n", GetCurrentThreadId());
    dbghelp::GetCurrentThreadCallstack(s);
    s.Append("\r\n");
#endif
    s.Append(gModulesInfo);

    s.Append("\r\n");
    s.Append(gAdditionalInfo->LendData());

    return s.StealData();
}

static void SendCrashInfo(char *s)
{
    lf("SendCrashInfo(): started");
    if (str::IsEmpty(s)) {
        plog("SendCrashInfo(): s is empty");
        return;
    }

    char *boundary = "0xKhTmLbOuNdArY";
    str::Str<char> headers(256, gCrashHandlerAllocator);
    headers.AppendFmt("Content-Type: multipart/form-data; boundary=%s", boundary);

    str::Str<char> data(2048, gCrashHandlerAllocator);
    data.AppendFmt("--%s\r\n", boundary);
    data.Append("Content-Disposition: form-data; name=\"file\"; filename=\"test.bin\"\r\n\r\n");
    data.Append(s);
    data.Append("\r\n");
    data.AppendFmt("\r\n--%s--\r\n", boundary);

    HttpPost(CRASH_SUBMIT_SERVER, CRASH_SUBMIT_URL, &headers, &data);
}

// We might have symbol files for older builds. If we're here, then we
// didn't get the symbols so we assume it's because symbols didn't match
// Returns false if files were there but we couldn't delete them
static bool DeleteSymbolsIfExist()
{
    bool ok1 = file::Delete(gLibMupdfPdbPath);
    bool ok2 = file::Delete(gSumatraPdfPdbPath);
    bool ok3 = file::Delete(gInstallerPdbPath);
    bool ok = ok1 && ok2 && ok3;
    if (!ok)
        plog("DeleteSymbolsIfExist() failed to delete");
    return ok;
}

// static (single .exe) build
static bool UnpackStaticSymbols(const TCHAR *pdbZipPath, const TCHAR *symDir)
{
    lf(_T("UnpackStaticSymbols(): unpacking %s to dir %s"), pdbZipPath, symDir);
    ZipFile archive(pdbZipPath, gCrashHandlerAllocator);
    if (!archive.UnzipFile(_T("SumatraPDF.pdb"), symDir)) {
        plog("Failed to unzip SumatraPDF.pdb");
        return false;
    }
    return true;
}

// lib (.exe + libmupdf.dll) release and pre-release builds
static bool UnpackLibSymbols(const TCHAR *pdbZipPath, const TCHAR *symDir)
{
    lf(_T("UnpackLibSymbols(): unpacking %s to dir %s"), pdbZipPath, symDir);
    ZipFile archive(pdbZipPath, gCrashHandlerAllocator);
    if (!archive.UnzipFile(_T("libmupdf.pdb"), symDir)) {
        plog("Failed to unzip libmupdf.pdb");
        return false;
    }
    if (!archive.UnzipFile(_T("SumatraPDF-no-MuPDF.pdb"), symDir)) {
        plog("Failed to unzip SumatraPDF-no-MuPDF.pdb");
        return false;
    }
    return true;
}

// an installer
static bool UnpackInstallerSymbols(const TCHAR *pdbZipPath, const TCHAR *symDir)
{
    lf(_T("UnpackInstallerSymbols(): unpacking %s to dir %s"), pdbZipPath, symDir);
    ZipFile archive(pdbZipPath, gCrashHandlerAllocator);
    if (!archive.UnzipFile(_T("Installer.pdb"), symDir)) {
        plog("Failed to unzip Installer.pdb");
        return false;
    }
    return true;
}

// .pdb files are stored in a .zip file on a web server. Download that .zip
// file as pdbZipPath, extract the symbols relevant to our executable
// to symDir directory.
// Returns false if downloading or extracting failed
// note: to simplify callers, it could choose pdbZipPath by itself (in a temporary
// directory) as the file is deleted on exit anyway
static bool DownloadAndUnzipSymbols(const TCHAR *pdbZipPath, const TCHAR *symDir)
{
    lf("DownloadAndUnzipSymbols() started");
    if (!symDir || !dir::Exists(symDir)) {
        plog("DownloadAndUnzipSymbols(): exiting because symDir doesn't exist");
        return false;
    }

    if (!DeleteSymbolsIfExist()) {
        plog("DownloadAndUnzipSymbols(): DeleteSymbolsIfExist() failed");
        return false;
    }

    if (!file::Delete(pdbZipPath)) {
        plog("DownloadAndUnzipSymbols(): deleting pdbZipPath failed");
        return false;
    }

#ifdef DEBUG
    // don't care about debug builds because we don't release them
    plog("DownloadAndUnzipSymbols(): DEBUG build so not doing anything");
    return false;
#else
    if (!HttpGetToFile(SYMBOL_DOWNLOAD_URL, pdbZipPath)) {
        plog("DownloadAndUnzipSymbols(): couldn't download symbols");
        return false;
    }

    unzSetAllocFuncs(&gUnzipAllocFuncs);

    bool ok = false;
    if (ExeSumatraStatic == gExeType) {
        ok = UnpackStaticSymbols(pdbZipPath, symDir);
    } else if (ExeSumatraLib == gExeType) {
        ok = UnpackLibSymbols(pdbZipPath, symDir);
    } else if (ExeInstaller == gExeType) {
        ok = UnpackInstallerSymbols(pdbZipPath, symDir);
    } else {
        plog("DownloadAndUnzipSymbols(): unknown exe type");
    }

    file::Delete(pdbZipPath);
    return ok;
#endif
}

// If we can't resolve the symbols, we assume it's because we don't have symbols
// so we'll try to download them and retry. If we can resolve symbols, we'll
// get the callstacks etc. and submit to our server for analysis.
void SubmitCrashInfo()
{
    if (!dir::Create(gSymbolsDir)) {
        plog("SubmitCrashInfo(): couldn't create symbols dir");
        return;
    }

    lf("SubmitCrashInfo(): start");
    lf(L"SubmitCrashInfo(): gSymbolPathW: '%s'", gSymbolPathW);
    if (!CrashHandlerCanUseNet()) {
        plog("SubmitCrashInfo(): internet access not allowed");
        return;
    }

    char *s = NULL;
    if (!dbghelp::Initialize(gSymbolPathW)) {
        plog("SubmitCrashInfo(): dbghelp::Initialize() failed");
        return;
    }

    if (!dbghelp::HasSymbols()) {
        if (!DownloadAndUnzipSymbols(gPdbZipPath, gSymbolsDir)) {
            plog("SubmitCrashInfo(): failed to download symbols");
            return;
        }

        if (!dbghelp::Initialize(gSymbolPathW, true)) {
            plog("SubmitCrashInfo(): second dbghelp::Initialize() failed");
            return;
        }
    }

    if (!dbghelp::HasSymbols()) {
        plog("SubmitCrashInfo(): HasSymbols() false after downloading symbols");
        return;
    }

    s = BuildCrashInfoText();
    if (!s)
        return;
    SendCrashInfo(s);
    gCrashHandlerAllocator->Free(s);
}

static DWORD WINAPI CrashDumpThread(LPVOID data)
{
    WaitForSingleObject(gDumpEvent, INFINITE);
    if (!dbghelp::Load())
        return 0;

#ifndef HAS_NO_SYMBOLS
    SubmitCrashInfo();
#endif
    // always write a MiniDump (for the latest crash only)
    // set the SUMATRAPDF_FULLDUMP environment variable for more complete dumps
    bool fullDump = (NULL != GetEnvironmentVariableA("SUMATRAPDF_FULLDUMP", NULL, 0));
    dbghelp::WriteMiniDump(gCrashDumpPath, &gMei, fullDump);
    return 0;
}

static LONG WINAPI DumpExceptionHandler(EXCEPTION_POINTERS *exceptionInfo)
{
    if (!exceptionInfo || (EXCEPTION_BREAKPOINT == exceptionInfo->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    static bool wasHere = false;
    if (wasHere)
        return EXCEPTION_CONTINUE_SEARCH; // Note: or should TerminateProcess()?
    wasHere = true;

    gMei.ThreadId = GetCurrentThreadId();
    gMei.ExceptionPointers = exceptionInfo;
    // per msdn (which is backed by my experience), MiniDumpWriteDump() doesn't
    // write callstack for the calling thread correctly. We use msdn-recommended
    // work-around of spinning a thread to do the writing
    SetEvent(gDumpEvent);
    WaitForSingleObject(gDumpThread, INFINITE);

    CrashHandlerMessage();
    TerminateProcess(GetCurrentProcess(), 1);

    return EXCEPTION_CONTINUE_SEARCH;
}

static char *OsNameFromVer(OSVERSIONINFOEX ver)
{
    if (VER_PLATFORM_WIN32_NT != ver.dwPlatformId)
        return "9x";
    if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 1)
        return "7"; // or Server 2008
    if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 0)
        return "Vista";
    if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 2)
        return "Server 2003";
    if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 1)
        return "XP";
    if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 0)
        return "2000";

    // either a newer or an older NT version, neither of which we support
    static char osVerStr[32];
    wsprintfA(osVerStr, "NT %d.%d", ver.dwMajorVersion, ver.dwMinorVersion);
    return osVerStr;
}

static void GetOsVersion(str::Str<char>& s)
{
    OSVERSIONINFOEX ver;
    ZeroMemory(&ver, sizeof(ver));
    ver.dwOSVersionInfoSize = sizeof(ver);
    BOOL ok = GetVersionEx((OSVERSIONINFO*)&ver);
    if (!ok)
        return;
    char *os = OsNameFromVer(ver);
    int servicePackMajor = ver.wServicePackMajor;
    int servicePackMinor = ver.wServicePackMinor;
    int buildNumber = ver.dwBuildNumber & 0xFFFF;
#ifdef _WIN64
    char *arch = "64-bit";
#else
    char *arch = IsRunningInWow64() ? "Wow64" : "32-bit";
#endif
    if (0 == servicePackMajor)
        s.AppendFmt("OS: Windows %s build %d %s\r\n", os, buildNumber, arch);
    else if (0 == servicePackMinor)
        s.AppendFmt("OS: Windows %s SP%d build %d %s\r\n", os, servicePackMajor, buildNumber, arch);
    else
        s.AppendFmt("OS: Windows %s %d.%d build %d %s\r\n", os, servicePackMajor, servicePackMinor, buildNumber, arch);
}

static void GetProcessorName(str::Str<char>& s)
{
    TCHAR *name = ReadRegStr(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DESCRIPTION\\System\\CentralProcessor"), _T("ProcessorNameString"));
    if (!name) // if more than one processor
        name = ReadRegStr(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"), _T("ProcessorNameString"));
    if (!name)
        return;

    ScopedMem<char> tmp(str::conv::ToUtf8(name));
    s.AppendFmt("Processor: %s\r\n", tmp);
    free(name);
}

static void GetMachineName(str::Str<char>& s)
{
    TCHAR *s1 = ReadRegStr(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DESCRIPTION\\System\\BIOS"), _T("SystemFamily"));
    TCHAR *s2 = ReadRegStr(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DESCRIPTION\\System\\BIOS"), _T("SystemVersion"));
    ScopedMem<char> s1u(s1 ? str::conv::ToUtf8(s1) : NULL);
    ScopedMem<char> s2u(s2 ? str::conv::ToUtf8(s2) : NULL);

    if (!s1u && !s2u)
        ; // pass
    else if (!s1u)
        s.AppendFmt("Machine: %s\r\n", s2u.Get());
    else if (!s2u || str::EqI(s1u, s2u))
        s.AppendFmt("Machine: %s\r\n", s1u.Get());
    else
        s.AppendFmt("Machine: %s %s\r\n", s1u.Get(), s2u.Get());

    free(s1);
    free(s2);
}

static void GetLanguage(str::Str<char>& s)
{
    char country[32] = { 0 }, lang[32] = { 0 };
    GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, country, dimof(country) - 1);
    GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO639LANGNAME, lang, dimof(lang) - 1);
    s.AppendFmt("Lang: %s %s\r\n", lang, country);
}

static void GetSystemInfo(str::Str<char>& s)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s.AppendFmt("Number Of Processors: %d\r\n", si.dwNumberOfProcessors);
    GetProcessorName(s);

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    float physMemGB   = (float)ms.ullTotalPhys     / (float)(1024 * 1024 * 1024);
    float totalPageGB = (float)ms.ullTotalPageFile / (float)(1024 * 1024 * 1024);
    DWORD usedPerc = ms.dwMemoryLoad;
    s.AppendFmt("Physical Memory: %.2f GB\r\nCommit Charge Limit: %.2f GB\r\nMemory Used: %d%%\r\n", physMemGB, totalPageGB, usedPerc);

    GetMachineName(s);
    GetLanguage(s);

    // Note: maybe more information, like:
    // * amount of memory used by Sumatra,
    // * graphics card and its driver version
    // * processor capabilities (mmx, sse, sse2 etc.)
}

static void GetModules(str::Str<char>& s)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 mod;
    mod.dwSize = sizeof(mod);
    BOOL cont = Module32First(snap, &mod);
    while (cont) {
        ScopedMem<char> nameA(str::conv::ToUtf8(mod.szModule));
        ScopedMem<char> pathA(str::conv::ToUtf8(mod.szExePath));
        s.AppendFmt("Module: %08X %06X %-16s %s\r\n", (DWORD)mod.modBaseAddr, (DWORD)mod.modBaseSize, nameA, pathA);
        cont = Module32Next(snap, &mod);
    }
    CloseHandle(snap);
}

static void BuildModulesInfo()
{
    str::Str<char> s(1024);
    GetModules(s);
    gModulesInfo = s.StealData();
}

static void BuildSystemInfo()
{
    str::Str<char> s(1024);
    GetProgramInfo(s);
    GetOsVersion(s);
    GetSystemInfo(s);
    gSystemInfo = s.StealData();
}

static bool StoreCrashDumpPaths(const TCHAR *symDir)
{
    if (!symDir)
        return false;
    gSymbolsDir = str::Dup(symDir);
    gPdbZipPath = path::Join(symDir, _T("symbols_tmp.zip"));
    gLibMupdfPdbPath = path::Join(symDir, _T("SumatraPDF.pdb"));
    gSumatraPdfPdbPath = path::Join(symDir, _T("libmupdf.pdb"));
    gInstallerPdbPath = path::Join(symDir, _T("Installer.pdb"));
    return true;
}

/* Setting symbol path:
add GetEnvironmentVariableA("_NT_SYMBOL_PATH", ..., ...)
add GetEnvironmentVariableA("_NT_ALTERNATE_SYMBOL_PATH", ..., ...)
add: "srv*c:\\symbols*http://msdl.microsoft.com/download/symbols;cache*c:\\symbols"
(except a better directory than c:\\symbols

Note: I've decided to use just one, known to me location rather than the
more comprehensive list. It works so why give dbghelp.dll more directories
to scan?
*/
static bool BuildSymbolPath()
{
    str::Str<WCHAR> path(1024);

#if 0
    WCHAR buf[512];
    DWORD res = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", buf, dimof(buf));
    if (0 < res && res < dimof(buf)) {
        path.Append(buf);
        path.Append(L";");
    }
    res = GetEnvironmentVariableW(L"_NT_ALTERNATE_SYMBOL_PATH", buf, dimof(buf));
    if (0 < res && res < dimof(buf)) {
        path.Append(buf);
        path.Append(L";");
    }
#endif

    path.Append(AsWStrQ(gSymbolsDir));
    //path.Append(_T(";"));
#if 0
    // this probably wouldn't work anyway because it requires symsrv.dll in the same directory
    // as dbghelp.dll and it's not present with the os-provided dbghelp.dll
    path.Append(L"srv*");
    path.Append(symDir);
    path.Append(L"*http://msdl.microsoft.com/download/symbols;cache*");
    path.Append(symDir);
#endif
#if 0
    // when running local builds, *.pdb is in the same dir as *.exe
    ScopedMem<TCHAR> exePath(GetExePath());
    path.AppendFmt(L"%s", AsWStrQ(exePath));
#endif
    gSymbolPathW = path.StealData();
    if (!gSymbolPathW)
        return false;
    return true;
}

// detect which exe it is (installer, sumatra static or sumatra with dlls)
static ExeType DetectExeType()
{
    ExeType exeType = ExeSumatraStatic;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        plog("DetectExeType(): failed to detect type");
        return exeType;
    }
    MODULEENTRY32 mod;
    mod.dwSize = sizeof(mod);
    BOOL cont = Module32First(snap, &mod);
    while (cont) {
        TCHAR *name = mod.szModule;
        if (str::EqI(name, _T("libmupdf.dll"))) {
            exeType = ExeSumatraLib;
            break;
        }
        if (str::StartsWithI(name, _T("SumatraPDF-")) && str::EndsWithI(name, _T("install.exe"))) {
            exeType = ExeInstaller;
            break;
        }
        cont = Module32Next(snap, &mod);
    }
    CloseHandle(snap);
    return exeType;
}

void CrashLogFmt(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *s = str::FmtV(fmt, args);
    gAdditionalInfo->AppendAndFree(s);
    va_end(args);
}

void InstallCrashHandler(const TCHAR *crashDumpPath, const TCHAR *symDir)
{
    assert(!gDumpEvent && !gDumpThread);

    if (!crashDumpPath)
        return;
    if (!StoreCrashDumpPaths(symDir))
        return;
    if (!BuildSymbolPath())
        return;
    BuildSystemInfo();
    // at this point list of modules should be complete (except
    // dbghlp.dll which shouldn't be loaded yet)
    BuildModulesInfo();

    gExeType = DetectExeType();
    // we pre-allocate as much as possible to minimize allocations
    // when crash handler is invoked. It's ok to use standard
    // allocation functions here.
    gCrashHandlerAllocator = new CrashHandlerAllocator();
    gCrashDumpPath = str::Dup(crashDumpPath);
    gAdditionalInfo = new str::Str<char>(4096);
    gDumpEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!gDumpEvent)
        return;
    gDumpThread = CreateThread(NULL, 0, CrashDumpThread, NULL, 0, 0);
    if (!gDumpThread)
        return;
    gPrevExceptionFilter = SetUnhandledExceptionFilter(DumpExceptionHandler);
}

void UninstallCrashHandler()
{
    if (gDumpEvent)
        SetUnhandledExceptionFilter(gPrevExceptionFilter);
    TerminateThread(gDumpThread, 1);
    CloseHandle(gDumpThread);
    CloseHandle(gDumpEvent);

    free(gCrashDumpPath);
    free(gSymbolsDir);
    free(gPdbZipPath);
    free(gLibMupdfPdbPath);
    free(gSumatraPdfPdbPath);
    free(gInstallerPdbPath);

    delete gAdditionalInfo;
    free(gSymbolPathW);
    free(gSystemInfo);
    free(gModulesInfo);
    delete gCrashHandlerAllocator;
}
