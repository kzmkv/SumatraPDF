/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"
#include "FileUtil.h"

namespace path {

static inline bool IsSep(WCHAR c)
{
    return L'\\' == c || L'/' == c;
}

static inline bool IsSep(char c)
{
    return '\\' == c || '/' == c;
}

// Note: returns pointer inside <path>, do not free
const WCHAR *GetBaseName(const WCHAR *path)
{
    const WCHAR *fileBaseName = path + str::Len(path);
    for (; fileBaseName > path; fileBaseName--)
        if (IsSep(fileBaseName[-1]))
            break;
    return fileBaseName;
}

// Note: returns pointer inside <path>, do not free
const char *GetBaseName(const char *path)
{
    const char *fileBaseName = path + str::Len(path);
    for (; fileBaseName > path; fileBaseName--)
        if (IsSep(fileBaseName[-1]))
            break;
    return fileBaseName;
}

// Note: returns pointer inside <path>, do not free
const TCHAR *GetExt(const TCHAR *path)
{
    const TCHAR *ext = path + str::Len(path);
    for (; ext > path && !IsSep(*ext); ext--)
        if (*ext == '.')
            return ext;
    return path + str::Len(path);
}

// Caller has to free
TCHAR *GetDir(const TCHAR *path)
{
    const TCHAR *baseName = GetBaseName(path);
    if (baseName == path) // relative directory
        return str::Dup(_T("."));
    if (baseName == path + 1) // relative root
        return str::DupN(path, 1);
    if (baseName == path + 3 && path[1] == ':') // local drive root
        return str::DupN(path, 3);
    if (baseName == path + 2 && str::StartsWith(path, _T("\\\\"))) // server root
        return str::Dup(path);
    // any subdirectory
    return str::DupN(path, baseName - path - 1);
}

TCHAR *Join(const TCHAR *path, const TCHAR *filename)
{
    if (IsSep(*filename))
        filename++;
    TCHAR *sep = NULL;
    if (!IsSep(path[str::Len(path) - 1]))
        sep = _T("\\");
    return str::Join(path, sep, filename);
}

// Normalize a file path.
//  remove relative path component (..\ and .\),
//  replace slashes by backslashes,
//  convert to long form.
//
// Returns a pointer to a memory allocated block containing the normalized string.
//   The caller is responsible for freeing the block.
//   Returns NULL if the file does not exist or if a memory allocation fails.
//
// Precondition: the file must exist on the file system.
//
// Note:
//   - the case of the root component is preserved
//   - the case of rest is set to the way it is stored on the file system
//
// e.g. suppose the a file "C:\foo\Bar.Pdf" exists on the file system then
//    "c:\foo\bar.pdf" becomes "c:\foo\Bar.Pdf"
//    "C:\foo\BAR.PDF" becomes "C:\foo\Bar.Pdf"
TCHAR *Normalize(const TCHAR *path)
{
    // convert to absolute path, change slashes into backslashes
    DWORD cch = GetFullPathName(path, 0, NULL, NULL);
    if (!cch)
        return str::Dup(path);
    ScopedMem<TCHAR> fullpath(SAZA(TCHAR, cch));
    GetFullPathName(path, cch, fullpath, NULL);
    // convert to long form
    cch = GetLongPathName(fullpath, NULL, 0);
    if (!cch)
        return fullpath.StealData();
    TCHAR *normpath = SAZA(TCHAR, cch);
    GetLongPathName(fullpath, normpath, cch);
    return normpath;
}

// Normalizes the file path and the converts it into a short form that
// can be used for interaction with non-UNICODE aware applications
TCHAR *ShortPath(const TCHAR *path)
{
    ScopedMem<TCHAR> normpath(Normalize(path));
    DWORD cch = GetShortPathName(normpath, NULL, 0);
    if (!cch)
        return normpath;
    TCHAR *shortpath = SAZA(TCHAR, cch);
    GetShortPathName(normpath, shortpath, cch);
    return shortpath;
}

// Code adapted from http://stackoverflow.com/questions/562701/best-way-to-determine-if-two-path-reference-to-same-file-in-c-c/562830#562830
// Determine if 2 paths point ot the same file...
bool IsSame(const TCHAR *path1, const TCHAR *path2)
{
    bool isSame = false, needFallback = true;
    HANDLE handle1 = CreateFile(path1, 0, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE handle2 = CreateFile(path2, 0, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

    if (handle1 != INVALID_HANDLE_VALUE && handle2 != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION fi1, fi2;
        if (GetFileInformationByHandle(handle1, &fi1) && GetFileInformationByHandle(handle2, &fi2)) {
            isSame = fi1.dwVolumeSerialNumber == fi2.dwVolumeSerialNumber &&
                     fi1.nFileIndexHigh == fi2.nFileIndexHigh &&
                     fi1.nFileIndexLow == fi2.nFileIndexLow;
            needFallback = false;
        }
    }

    CloseHandle(handle1);
    CloseHandle(handle2);

    if (!needFallback)
        return isSame;

    ScopedMem<TCHAR> npath1(Normalize(path1));
    ScopedMem<TCHAR> npath2(Normalize(path2));
    // consider the files different, if their paths can't be normalized
    if (!npath1 || !npath2)
        return false;
    return str::EqI(npath1, npath2);
}

bool HasVariableDriveLetter(const TCHAR *path)
{
    TCHAR root[] = _T("?:\\");
    root[0] = _totupper(path[0]);
    if (root[0] < 'A' || 'Z' < root[0])
        return false;

    UINT driveType = GetDriveType(root);
    return DRIVE_REMOVABLE == driveType ||
           DRIVE_CDROM == driveType ||
           DRIVE_NO_ROOT_DIR == driveType;
}

static bool MatchWildcardsRec(const TCHAR *filename, const TCHAR *filter)
{
#define AtEndOf(str) (*(str) == '\0')
    switch (*filter) {
    case '\0': case ';':
        return AtEndOf(filename);
    case '*':
        filter++;
        while (!AtEndOf(filename) && !MatchWildcardsRec(filename, filter))
            filename++;
        return !AtEndOf(filename) || AtEndOf(filter) || *filter == ';';
    case '?':
        return !AtEndOf(filename) && MatchWildcardsRec(filename + 1, filter + 1);
    default:
        return _totlower(*filename) == _totlower(*filter) &&
               MatchWildcardsRec(filename + 1, filter + 1);
    }
#undef AtEndOf
}

/* matches the filename of a path against a list of semicolon
   separated filters as used by the common file dialogs
   (e.g. "*.pdf;*.xps;?.*" will match all PDF and XPS files and
   all filenames consisting of only a single character and
   having any extension) */
bool Match(const TCHAR *path, const TCHAR *filter)
{
    path = GetBaseName(path);
    while (str::FindChar(filter, ';')) {
        if (MatchWildcardsRec(path, filter))
            return true;
        filter = str::FindChar(filter, ';') + 1;
    }
    return MatchWildcardsRec(path, filter);
}

// returns the path to either the %TEMP% directory or a
// non-existing file inside whose name starts with filePrefix
TCHAR *GetTempPath(const TCHAR *filePrefix)
{
    TCHAR tempDir[MAX_PATH - 14];
    DWORD res = ::GetTempPath(dimof(tempDir), tempDir);
    if (!res || res >= dimof(tempDir))
        return NULL;
    if (!filePrefix)
        return str::Dup(tempDir);
    TCHAR path[MAX_PATH];
    if (!GetTempFileName(tempDir, filePrefix, 0, path))
        return NULL;
    return str::Dup(path);
}

}

namespace file {

bool Exists(const TCHAR *filePath)
{
    if (NULL == filePath)
        return false;

    WIN32_FILE_ATTRIBUTE_DATA   fileInfo;
    BOOL res = GetFileAttributesEx(filePath, GetFileExInfoStandard, &fileInfo);
    if (0 == res)
        return false;

    if (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    return true;
}

size_t GetSize(const TCHAR *filePath)
{
    ScopedHandle h(CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (h == INVALID_HANDLE_VALUE)
        return INVALID_FILE_SIZE;

    // Don't use GetFileAttributesEx to retrieve the file size, as
    // that function doesn't interact well with symlinks, etc.
    LARGE_INTEGER lsize;
    BOOL ok = GetFileSizeEx(h, &lsize);
    if (!ok)
        return INVALID_FILE_SIZE;

#ifdef _WIN64
    return lsize.QuadPart;
#else
    if (lsize.HighPart > 0)
        return INVALID_FILE_SIZE;
    return lsize.LowPart;
#endif
}

char *ReadAll(const TCHAR *filePath, size_t *fileSizeOut)
{
    size_t size = GetSize(filePath);
    if (INVALID_FILE_SIZE == size)
        return NULL;

    // overflow check
    if (size + sizeof(WCHAR) < sizeof(WCHAR))
        return NULL;
    /* allocate one byte more and 0-terminate just in case it's a text
       file we'll want to treat as C string. Doesn't hurt for binary
       files (note: two byte terminator for UTF-16 files) */
    char *data = SAZA(char, size + sizeof(WCHAR));
    if (!data)
        return NULL;

    if (!ReadAll(filePath, data, size)) {
        free(data);
        return NULL;
    }

    if (fileSizeOut)
        *fileSizeOut = size;
    return data;
}

bool ReadAll(const TCHAR *filePath, char *buffer, size_t bufferLen)
{
    ScopedHandle h(CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD sizeRead;
    BOOL ok = ReadFile(h, buffer, (DWORD)bufferLen, &sizeRead, NULL);
    return ok && sizeRead == bufferLen;
}

bool WriteAll(const TCHAR *filePath, const void *data, size_t dataLen)
{
    ScopedHandle h(CreateFile(filePath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (INVALID_HANDLE_VALUE == h)
        return false;

    DWORD size;
    BOOL ok = WriteFile(h, data, (DWORD)dataLen, &size, NULL);
    assert(!ok || (dataLen == (size_t)size));
    return ok && dataLen == (size_t)size;
}

// Return true if the file wasn't there or was successfully deleted
bool Delete(const TCHAR *filePath)
{
    BOOL ok = DeleteFile(filePath);
    return ok || GetLastError() == ERROR_FILE_NOT_FOUND;
}

FILETIME GetModificationTime(const TCHAR *filePath)
{
    FILETIME lastMod = { 0 };
    ScopedHandle h(CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (h != INVALID_HANDLE_VALUE)
        GetFileTime(h, NULL, NULL, &lastMod);
    return lastMod;
}

bool SetModificationTime(const TCHAR *filePath, FILETIME lastMod)
{
    ScopedHandle h(CreateFile(filePath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL));
    if (INVALID_HANDLE_VALUE == h)
        return false;
    return SetFileTime(h, NULL, NULL, &lastMod);
}

bool StartsWith(const TCHAR *filePath, const char *magicNumber, size_t len)
{
    if (len == (size_t)-1)
        len = str::Len(magicNumber);
    ScopedMem<char> header(SAZA(char, len));
    if (!header)
        return false;

    ReadAll(filePath, header, len);
    return memeq(header, magicNumber, len);
}

int GetZoneIdentifier(const TCHAR *filePath)
{
    ScopedMem<TCHAR> path(str::Join(filePath, _T(":Zone.Identifier")));
    return GetPrivateProfileInt(_T("ZoneTransfer"), _T("ZoneId"), URLZONE_INVALID, path);
}

bool SetZoneIdentifier(const TCHAR *filePath, int zoneId)
{
    ScopedMem<TCHAR> path(str::Join(filePath, _T(":Zone.Identifier")));
    ScopedMem<TCHAR> id(str::Format(_T("%d"), zoneId));
    return WritePrivateProfileString(_T("ZoneTransfer"), _T("ZoneId"), id, path);
}

}

namespace dir {

bool Exists(const TCHAR *dir)
{
    if (NULL == dir)
        return false;

    WIN32_FILE_ATTRIBUTE_DATA   fileInfo;
    BOOL res = GetFileAttributesEx(dir, GetFileExInfoStandard, &fileInfo);
    if (0 == res)
        return false;

    if (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return true;
    return false;
}

// Return true if a directory already exists or has been successfully created
bool Create(const TCHAR *dir)
{
    BOOL ok = CreateDirectory(dir, NULL);
    if (ok)
        return true;
    return ERROR_ALREADY_EXISTS == GetLastError();
}

// creates a directory and all its parent directories that don't exist yet
bool CreateAll(const TCHAR *dir)
{
    ScopedMem<TCHAR> parent(path::GetDir(dir));
    if (!str::Eq(parent, dir) && !Exists(parent))
        CreateAll(parent);
    return Create(dir);
}

}
