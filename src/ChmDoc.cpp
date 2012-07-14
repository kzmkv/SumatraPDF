/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include "ChmDoc.h"

#include "ByteReader.h"
#include "FileUtil.h"
#include "TrivialHtmlParser.h"

#define CHM_MT
#ifdef UNICODE
#define PPC_BSTR
#endif
#include <chm_lib.h>

ChmDoc::~ChmDoc()
{
    chm_close(chmHandle);
}

bool ChmDoc::HasData(const char *fileName)
{
    if (!fileName)
        return NULL;

    ScopedMem<char> tmpName;
    if (!str::StartsWith(fileName, "/")) {
        tmpName.Set(str::Join("/", fileName));
        fileName = tmpName;
    }
    else if (str::StartsWith(fileName, "///"))
        fileName += 2;

    struct chmUnitInfo info;
    return chm_resolve_object(chmHandle, fileName, &info) == CHM_RESOLVE_SUCCESS;
}

unsigned char *ChmDoc::GetData(const char *fileName, size_t *lenOut)
{
    ScopedMem<char> fileNameTmp;
    if (!str::StartsWith(fileName, "/")) {
        fileNameTmp.Set(str::Join("/", fileName));
        fileName = fileNameTmp;
    } else if (str::StartsWith(fileName, "///")) {
        fileName += 2;
    }

    struct chmUnitInfo info;
    int res = chm_resolve_object(chmHandle, fileName, &info);
    if (CHM_RESOLVE_SUCCESS != res)
        return NULL;
    size_t len = (size_t)info.length;
    if (len > 128 * 1024 * 1024) {
        // don't allow anything above 128 MB
        return NULL;
    }

    // +1 for 0 terminator for C string compatibility
    ScopedMem<unsigned char> data((unsigned char *)malloc(len + 1));
    if (!data)
        return NULL;
    if (!chm_retrieve_object(chmHandle, &info, data.Get(), 0, len))
        return NULL;
    data[len] = '\0';

    if (lenOut)
        *lenOut = len;
    return data.StealData();
}

char *ChmDoc::ToUtf8(const unsigned char *text)
{
    const char *s = (char *)text;
    if (str::StartsWith(s, UTF8_BOM))
        return str::Dup(s + 3);
    if (CP_UTF8 == codepage)
        return str::Dup(s);
    return str::ToMultiByte(s, codepage, CP_UTF8);
}

TCHAR *ChmDoc::ToStr(const char *text)
{
    return str::conv::FromCodePage(text, codepage);
}

static char *GetCharZ(const unsigned char *data, size_t len, size_t off)
{
    if (off >= len)
        return NULL;
    CrashIf(!memchr(data + off, '\0', len - off + 1)); // data is zero-terminated
    const char *str = (char *)data + off;
    if (str::IsEmpty(str))
        return NULL;
    return str::Dup(str);
}

// http://www.nongnu.org/chmspec/latest/Internal.html#WINDOWS
void ChmDoc::ParseWindowsData()
{
    size_t windowsLen, stringsLen;
    ScopedMem<unsigned char> windowsData(GetData("/#WINDOWS", &windowsLen));
    ScopedMem<unsigned char> stringsData(GetData("/#STRINGS", &stringsLen));
    if (!windowsData || !stringsData)
        return;
    if (windowsLen <= 8)
        return;

    ByteReader rw(windowsData, windowsLen);
    DWORD entries = rw.DWordLE(0);
    DWORD entrySize = rw.DWordLE(4);
    if (entrySize < 188)
        return;

    for (DWORD i = 0; i < entries && (i + 1) * entrySize <= windowsLen; i++) {
        DWORD off = 8 + i * entrySize;
        if (!title) {
            DWORD strOff = rw.DWordLE(off + 0x14);
            title.Set(GetCharZ(stringsData, stringsLen, strOff));
        }
        if (!tocPath) {
            DWORD strOff = rw.DWordLE(off + 0x60);
            tocPath.Set(GetCharZ(stringsData, stringsLen, strOff));
        }
        if (!indexPath) {
            DWORD strOff = rw.DWordLE(off + 0x64);
            indexPath.Set(GetCharZ(stringsData, stringsLen, strOff));
        }
        if (!homePath) {
            DWORD strOff = rw.DWordLE(off + 0x68);
            homePath.Set(GetCharZ(stringsData, stringsLen, strOff));
        }
    }
}

// http://www.nongnu.org/chmspec/latest/Internal.html#SYSTEM
bool ChmDoc::ParseSystemData()
{
    size_t dataLen;
    ScopedMem<unsigned char> data(GetData("/#SYSTEM", &dataLen));
    if (!data)
        return false;

    ByteReader r(data, dataLen);
    DWORD len = 0;
    // Note: skipping DWORD version at offset 0. It's supposed to be 2 or 3.
    for (size_t off = 4; off + 4 < dataLen; off += len + 4) {
        // Note: at some point we seem to get off-sync i.e. I'm seeing
        // many entries with type == 0 and len == 0. Seems harmless.
        len = r.WordLE(off + 2);
        if (len == 0)
            continue;
        WORD type = r.WordLE(off);
        switch (type) {
        case 0:
            if (!tocPath)
                tocPath.Set(GetCharZ(data, dataLen, off + 4));
            break;
        case 1:
            if (!indexPath)
                indexPath.Set(GetCharZ(data, dataLen, off + 4));
            break;
        case 2:
            if (!homePath)
                homePath.Set(GetCharZ(data, dataLen, off + 4));
            break;
        case 3:
            if (!title)
                title.Set(GetCharZ(data, dataLen, off + 4));
            break;
        case 6:
            // compiled file - ignore
            break;
        case 9:
            if (!creator)
                creator.Set(GetCharZ(data, dataLen, off + 4));
            break;
        case 16:
            // default font - ignore
            break;
        }
    }

    return true;
}

static UINT GetChmCodepage(const TCHAR *fileName)
{
    // cf. http://msdn.microsoft.com/en-us/library/bb165625(v=VS.90).aspx
    static struct {
        DWORD langId;
        UINT codepage;
    } langIdToCodepage[] = {
        { 1025, 1256 }, { 2052,  936 }, { 1028,  950 }, { 1029, 1250 },
        { 1032, 1253 }, { 1037, 1255 }, { 1038, 1250 }, { 1041,  932 },
        { 1042,  949 }, { 1045, 1250 }, { 1049, 1251 }, { 1051, 1250 },
        { 1060, 1250 }, { 1055, 1254 }
    };

    char header[24];
    if (!file::ReadAll(fileName, header, sizeof(header)))
        return CP_CHM_DEFAULT;

    DWORD lang_id = ByteReader(header, sizeof(header)).DWordLE(20);
    for (int i = 0; i < dimof(langIdToCodepage); i++)
        if (lang_id == langIdToCodepage[i].langId)
            return langIdToCodepage[i].codepage;

    return CP_CHM_DEFAULT;
}

bool ChmDoc::Load(const TCHAR *fileName)
{
    chmHandle = chm_open((TCHAR *)fileName);
    if (!chmHandle)
        return false;

    ParseWindowsData();
    if (!ParseSystemData())
        return false;

    if (!HasData(homePath)) {
        const char *pathsToTest[] = {
            "/index.htm", "/index.html", "/default.htm", "/default.html"
        };
        for (int i = 0; i < dimof(pathsToTest); i++) {
            if (HasData(pathsToTest[i]))
                homePath.Set(str::Dup(pathsToTest[i]));
        }
    }
    if (!HasData(homePath))
        return false;

    codepage = GetChmCodepage(fileName);
    if (GetACP() == codepage)
        codepage = CP_ACP;

    return true;
}

TCHAR *ChmDoc::GetProperty(const char *name)
{
    ScopedMem<TCHAR> result;
    if (str::Eq(name, "Title") && title)
        result.Set(str::conv::FromCodePage(title, codepage));
    else if (str::Eq(name, "Creator") && creator)
        result.Set(str::conv::FromCodePage(creator, codepage));
    // TODO: shouldn't it be up to the front-end to normalize whitespace?
    if (result) {
        // TODO: original code called str::RemoveChars(result, "\n\r\t")
        str::NormalizeWS(result);
    }
    return result.StealData();
}

const char *ChmDoc::GetHomePath()
{
    return homePath;
}

static int ChmEnumerateEntry(struct chmFile *chmHandle, struct chmUnitInfo *info, void *data)
{
    if (info->path) {
        Vec<char *> *paths = (Vec<char *> *)data;
        paths->Append(str::Dup(info->path));
    }
    return CHM_ENUMERATOR_CONTINUE;
}

Vec<char *> *ChmDoc::GetAllPaths()
{
    Vec<char *> *paths = new Vec<char *>();
    chm_enumerate(chmHandle, CHM_ENUMERATE_FILES | CHM_ENUMERATE_NORMAL, ChmEnumerateEntry, paths);
    return paths;
}

/* The html looks like:
<li>
  <object type="text/sitemap">
    <param name="Name" value="Main Page">
    <param name="Local" value="0789729717_main.html">
    <param name="ImageNumber" value="12">
  </object>
  <ul> ... children ... </ul>
<li>
  ... siblings ...
*/
static bool VisitChmTocItem(EbookTocVisitor *visitor, HtmlElement *el, UINT cp, int level)
{
    assert(str::Eq("li", el->name));
    el = el->GetChildByName("object");
    if (!el)
        return false;

    ScopedMem<TCHAR> name, local;
    for (el = el->GetChildByName("param"); el; el = el->next) {
        if (!str::Eq("param", el->name))
            continue;
        ScopedMem<TCHAR> attrName(el->GetAttribute("name"));
        ScopedMem<TCHAR> attrVal(el->GetAttribute("value"));
        if (!attrName || !attrVal)
            /* ignore incomplete/unneeded <param> */;
        else if (str::EqI(attrName, _T("Name"))) {
#ifdef UNICODE
            if (cp != CP_CHM_DEFAULT) {
                ScopedMem<char> bytes(str::conv::ToCodePage(attrVal, CP_CHM_DEFAULT));
                attrVal.Set(str::conv::FromCodePage(bytes, cp));
            }
#endif
            name.Set(attrVal.StealData());
        }
        else if (str::EqI(attrName, _T("Local"))) {
            // remove the ITS protocol and any filename references from the URLs
            if (str::Find(attrVal, _T("::/")))
                attrVal.Set(str::Dup(str::Find(attrVal, _T("::/")) + 3));
            local.Set(attrVal.StealData());
        }
    }
    if (!name)
        return false;

    visitor->visit(name, local, level);
    return true;
}

/* The html looks like:
<li>
  <object type="text/sitemap">
    <param name="Keyword" value="- operator">
    <param name="Name" value="Subtraction Operator (-)">
    <param name="Local" value="html/vsoprsubtract.htm">
    <param name="Name" value="Subtraction Operator (-)">
    <param name="Local" value="html/js56jsoprsubtract.htm">
  </object>
  <ul> ... optional children ... </ul>
<li>
  ... siblings ...
*/
static bool VisitChmIndexItem(EbookTocVisitor *visitor, HtmlElement *el, UINT cp, int level)
{
    assert(str::Eq("li", el->name));
    el = el->GetChildByName("object");
    if (!el)
        return false;

    StrVec references;
    ScopedMem<TCHAR> keyword, name;
    for (el = el->GetChildByName("param"); el; el = el->next) {
        if (!str::Eq("param", el->name))
            continue;
        ScopedMem<TCHAR> attrName(el->GetAttribute("name"));
        ScopedMem<TCHAR> attrVal(el->GetAttribute("value"));
        if (!attrName || !attrVal)
            /* ignore incomplete/unneeded <param> */;
        else if (str::EqI(attrName, _T("Keyword"))) {
#ifdef UNICODE
            if (cp != CP_CHM_DEFAULT) {
                ScopedMem<char> bytes(str::conv::ToCodePage(attrVal, CP_CHM_DEFAULT));
                attrVal.Set(str::conv::FromCodePage(bytes, cp));
            }
#endif
            keyword.Set(attrVal.StealData());
        }
        else if (str::EqI(attrName, _T("Name"))) {
#ifdef UNICODE
            if (cp != CP_CHM_DEFAULT) {
                ScopedMem<char> bytes(str::conv::ToCodePage(attrVal, CP_CHM_DEFAULT));
                attrVal.Set(str::conv::FromCodePage(bytes, cp));
            }
#endif
            name.Set(attrVal.StealData());
            // some CHM documents seem to use a lonely Name instead of Keyword
            if (!keyword)
                keyword.Set(str::Dup(name));
        }
        else if (str::EqI(attrName, _T("Local")) && name) {
            // remove the ITS protocol and any filename references from the URLs
            if (str::Find(attrVal, _T("::/")))
                attrVal.Set(str::Dup(str::Find(attrVal, _T("::/")) + 3));
            references.Append(name.StealData());
            references.Append(attrVal.StealData());
        }
    }
    if (!keyword)
        return false;

    if (references.Count() == 2) {
        visitor->visit(keyword, references.At(1), level);
        return true;
    }
    visitor->visit(keyword, NULL, level);
    for (size_t i = 0; i < references.Count(); i += 2) {
        visitor->visit(references.At(i), references.At(i + 1), level + 1);
    }
    return true;
}

static void WalkChmTocOrIndex(EbookTocVisitor *visitor, HtmlElement *list, UINT cp, bool isIndex, int level=1)
{
    assert(str::Eq("ul", list->name));

    // some broken ToCs wrap every <li> into its own <ul>
    for (; list && str::Eq(list->name, "ul"); list = list->next) {
        for (HtmlElement *el = list->down; el; el = el->next) {
            if (!str::Eq(el->name, "li"))
                continue; // ignore unexpected elements
            bool valid = (isIndex ? VisitChmIndexItem : VisitChmTocItem)(visitor, el, cp, level);
            if (!valid)
                continue; // skip incomplete elements and all their children

            HtmlElement *nested = el->GetChildByName("ul");
            // some broken ToCs have the <ul> follow right *after* a <li>
            if (!nested && el->next && str::Eq(el->next->name, "ul"))
                nested = el->next;
            if (nested)
                WalkChmTocOrIndex(visitor, nested, cp, isIndex, level + 1);
        }
    }
}

static bool ParseTocOrIndex(EbookTocVisitor *visitor, const char *html, UINT codepage, bool isIndex)
{
    HtmlParser p;
    UINT cp = codepage;
    // detect UTF-8 content by BOM
    if (str::StartsWith(html, UTF8_BOM)) {
        html += 3;
        cp = CP_UTF8;
    }
    // enforce the default codepage, so that pre-encoded text and
    // entities are in the same codepage and VisitChmTocItem yields
    // consistent results
    HtmlElement *el = p.Parse(html, CP_CHM_DEFAULT);
    if (!el)
        return false;
    el = p.FindElementByName("body");
    // since <body> is optional, also continue without one
    el = p.FindElementByName("ul", el);
    if (!el)
        return false;
    WalkChmTocOrIndex(visitor, el, cp, isIndex);
    return true;
}

bool ChmDoc::HasToc() const
{
    return tocPath != NULL;
}

bool ChmDoc::ParseToc(EbookTocVisitor *visitor)
{
    if (!tocPath)
        return false;
    ScopedMem<unsigned char> htmlData(GetData(tocPath, NULL));
    const char *html = (char *)htmlData.Get();
    if (!html)
        return false;
    return ParseTocOrIndex(visitor, html, codepage, false);
}

bool ChmDoc::HasIndex() const
{
    return indexPath != NULL;
}

bool ChmDoc::ParseIndex(EbookTocVisitor *visitor)
{
    if (!indexPath)
        return false;
    ScopedMem<unsigned char> htmlData(GetData(indexPath, NULL));
    const char *html = (char *)htmlData.Get();
    if (!html)
        return false;
    return ParseTocOrIndex(visitor, html, codepage, true);
}

bool ChmDoc::IsSupportedFile(const TCHAR *fileName, bool sniff)
{
    if (sniff)
        return file::StartsWith(fileName, "ITSF");

    return str::EndsWithI(fileName, _T(".chm"));
}

ChmDoc *ChmDoc::CreateFromFile(const TCHAR *fileName)
{
    ChmDoc *doc = new ChmDoc();
    if (!doc || !doc->Load(fileName)) {
        delete doc;
        return NULL;
    }
    return doc;
}
