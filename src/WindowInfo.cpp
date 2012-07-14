/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include "WindowInfo.h"

#include "FileUtil.h"
#include "FileWatch.h"
#include "Notifications.h"
#include "PdfSync.h"
#include "Print.h"
#include "Resource.h"
#include "Selection.h"
#include "StressTesting.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "WinUtil.h"

WindowInfo::WindowInfo(HWND hwnd) :
    dm(NULL), menu(NULL), hwndFrame(hwnd),
    linkOnLastButtonDown(NULL), url(NULL), selectionOnPage(NULL),
    tocLoaded(false), tocVisible(false), tocRoot(NULL), tocKeepSelection(false),
    fullScreen(false), presentation(PM_DISABLED), tocBeforeFullScreen(false),
    windowStateBeforePresentation(0), prevStyle(0),
    hwndCanvas(NULL), hwndToolbar(NULL), hwndReBar(NULL),
    hwndFindText(NULL), hwndFindBox(NULL), hwndFindBg(NULL),
    hwndPageText(NULL), hwndPageBox(NULL), hwndPageBg(NULL), hwndPageTotal(NULL),
    hwndTocBox(NULL), hwndTocTree(NULL),
    hwndSidebarSplitter(NULL), hwndFavSplitter(NULL),
    hwndInfotip(NULL), infotipVisible(false),
    findThread(NULL), findCanceled(false), printThread(NULL), printCanceled(false),
    showSelection(false), mouseAction(MA_IDLE), dragStartPending(false),
    prevZoomVirtual(INVALID_ZOOM), prevDisplayMode(DM_AUTOMATIC),
    loadedFilePath(NULL), currPageNo(0),
    xScrollSpeed(0), yScrollSpeed(0), wheelAccumDelta(0),
    delayedRepaintTimer(0), watcher(NULL),
    pdfsync(NULL), stressTest(NULL),
    hwndFavBox(NULL), hwndFavTree(NULL)
{
    ZeroMemory(&selectionRect, sizeof(selectionRect));

    dpi = win::GetHwndDpi(hwndFrame, &uiDPIFactor);
    touchState.panStarted = false;
    buffer = new DoubleBuffer(hwndCanvas, canvasRc);
    linkHandler = new LinkHandler(*this);
    notifications = new Notifications();
    fwdSearchMark.show = false;
}

WindowInfo::~WindowInfo()
{
    delete stressTest;

    delete pdfsync;
    delete watcher;
    delete linkHandler;
    delete buffer;
    delete selectionOnPage;
    delete linkOnLastButtonDown;
    delete tocRoot;
    delete notifications;
    // delete DisplayModel/BaseEngine last, as e.g.
    // DocTocItem or PageElement might still need the
    // BaseEngine in their destructors
    delete dm;

    free(loadedFilePath);
}

// Notify both display model and double-buffer (if they exist)
// about a potential change of available canvas size
void WindowInfo::UpdateCanvasSize()
{
    RectI rc = ClientRect(hwndCanvas);
    if (canvasRc == rc)
        return;
    canvasRc = rc;

    // create a new output buffer and notify the model
    // about the change of the canvas size
    delete buffer;
    buffer = new DoubleBuffer(hwndCanvas, canvasRc);

    if (IsDocLoaded()) {
        // the display model needs to know the full size (including scroll bars)
        dm->ChangeViewPortSize(GetViewPortSize());
    }

    // keep the notifications visible (only needed for right-to-left layouts)
    if (IsUIRightToLeft())
        notifications->Relayout();
}

SizeI WindowInfo::GetViewPortSize()
{
    SizeI size = canvasRc.Size();

    DWORD style = GetWindowLong(hwndCanvas, GWL_STYLE);
    if ((style & WS_VSCROLL))
        size.dx += GetSystemMetrics(SM_CXVSCROLL);
    if ((style & WS_HSCROLL))
        size.dy += GetSystemMetrics(SM_CYHSCROLL);

    return size;
}

void WindowInfo::RedrawAll(bool update)
{
    InvalidateRect(this->hwndCanvas, NULL, false);
    if (update)
        UpdateWindow(this->hwndCanvas);
}

void WindowInfo::ToggleZoom()
{
    assert(this->dm);
    if (!this->IsDocLoaded()) return;

    if (ZOOM_FIT_PAGE == this->dm->ZoomVirtual())
        this->dm->ZoomTo(ZOOM_FIT_WIDTH);
    else if (ZOOM_FIT_WIDTH == this->dm->ZoomVirtual())
        this->dm->ZoomTo(ZOOM_FIT_CONTENT);
    else
        this->dm->ZoomTo(ZOOM_FIT_PAGE);
}

void WindowInfo::MoveDocBy(int dx, int dy)
{
    assert(this->dm);
    if (!this->IsDocLoaded()) return;
    assert(!this->linkOnLastButtonDown);
    if (this->linkOnLastButtonDown) return;
    if (0 != dx)
        this->dm->ScrollXBy(dx);
    if (0 != dy)
        this->dm->ScrollYBy(dy, false);
}

#define MULTILINE_INFOTIP_WIDTH_PX 500

void WindowInfo::CreateInfotip(const TCHAR *text, RectI& rc, bool multiline)
{
    if (str::IsEmpty(text)) {
        this->DeleteInfotip();
        return;
    }

    TOOLINFO ti = { 0 };
    ti.cbSize = sizeof(ti);
    ti.hwnd = this->hwndCanvas;
    ti.uFlags = TTF_SUBCLASS;
    ti.lpszText = (TCHAR *)text;
    ti.rect = rc.ToRECT();

    if (multiline || str::FindChar(text, _T('\n')))
        SendMessage(this->hwndInfotip, TTM_SETMAXTIPWIDTH, 0, MULTILINE_INFOTIP_WIDTH_PX);
    else
        SendMessage(this->hwndInfotip, TTM_SETMAXTIPWIDTH, 0, -1);

    SendMessage(this->hwndInfotip, this->infotipVisible ? TTM_NEWTOOLRECT : TTM_ADDTOOL, 0, (LPARAM)&ti);
    this->infotipVisible = true;
}

void WindowInfo::DeleteInfotip()
{
    if (!infotipVisible)
        return;

    TOOLINFO ti = { 0 };
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwndCanvas;

    SendMessage(hwndInfotip, TTM_DELTOOL, 0, (LPARAM)&ti);
    infotipVisible = false;
}

void WindowInfo::LaunchBrowser(const TCHAR *url)
{
    ::LaunchBrowser(url);
}

void WindowInfo::FocusFrame(bool always)
{
    if (always || !FindWindowInfoByHwnd(GetFocus()))
        SetFocus(hwndFrame);
}

BaseEngine *LinkHandler::engine() const
{
    if (!owner || !owner->dm)
        return NULL;
    return owner->dm->engine;
}

void LinkHandler::GotoLink(PageDestination *link)
{
    assert(owner && owner->linkHandler == this);
    if (!link)
        return;
    if (!engine())
        return;

    DisplayModel *dm = owner->dm;
    ScopedMem<TCHAR> path(link->GetDestValue());
    PageDestType type = link->GetDestType();
    if (Dest_ScrollTo == type) {
        // TODO: respect link->ld.gotor.new_window for PDF documents ?
        ScrollTo(link);
    }
    else if (Dest_LaunchURL == type) {
        if (!path)
            /* ignore missing URLs */;
        else if (!str::FindChar(path, ':')) {
            // treat relative URIs as file paths
            // LaunchFile will reject unsupported file types
            LaunchFile(path, NULL);
        }
        else {
            // LaunchBrowser will reject unsupported URI schemes
            LaunchBrowser(path);
        }
    }
    else if (Dest_LaunchEmbedded == type) {
        // open embedded PDF documents in a new window
        if (path && str::StartsWith(path.Get(), dm->FilePath())) {
            WindowInfo *newWin = FindWindowInfoByFile(path);
            if (!newWin) {
                LoadArgs args(path, owner);
                newWin = LoadDocument(args);
            }
            if (newWin)
                newWin->Focus();
        }
        // offer to save other attachments to a file
        else
            link->SaveEmbedded(LinkSaver(*owner, path));
    }
    else if (Dest_LaunchFile == type) {
        if (path) {
            // LaunchFile only opens files inside SumatraPDF
            // (except for allowed perceived file types)
            LaunchFile(path, link);
        }
    }
    // predefined named actions
    else if (Dest_NextPage == type)
        dm->GoToNextPage(0);
    else if (Dest_PrevPage == type)
        dm->GoToPrevPage(0);
    else if (Dest_FirstPage == type)
        dm->GoToFirstPage();
    else if (Dest_LastPage == type)
        dm->GoToLastPage();
    // Adobe Reader extensions to the spec, cf. http://www.tug.org/applications/hyperref/manual.html
    else if (Dest_FindDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_FIND_FIRST, 0);
    else if (Dest_FullScreen == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_VIEW_PRESENTATION_MODE, 0);
    else if (Dest_GoBack == type)
        dm->Navigate(-1);
    else if (Dest_GoForward == type)
        dm->Navigate(1);
    else if (Dest_GoToPageDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_GOTO_PAGE, 0);
    else if (Dest_PrintDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_PRINT, 0);
    else if (Dest_SaveAsDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_SAVEAS, 0);
    else if (Dest_ZoomToDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_ZOOM_CUSTOM, 0);
    else
        CrashIf(Dest_None != type);
}

void LinkHandler::ScrollTo(PageDestination *dest)
{
    assert(owner && owner->linkHandler == this);
    int pageNo = dest->GetDestPageNo();
    if (pageNo <= 0)
        return;

    if (owner->dm->AsChmEngine()) {
        owner->dm->AsChmEngine()->GoToDestination(dest);
        return;
    }

    DisplayModel *dm = owner->dm;
    PointI scroll(-1, 0);
    RectD rect = dest->GetDestRect();

    if (rect.IsEmpty()) {
        // PDF: /XYZ top left
        // scroll to rect.TL()
        PointD scrollD = dm->engine->Transform(rect.TL(), pageNo, dm->ZoomReal(), dm->Rotation());
        scroll = scrollD.Convert<int>();

        // default values for the coordinates mean: keep the current position
        if (DEST_USE_DEFAULT == rect.x)
            scroll.x = -1;
        if (DEST_USE_DEFAULT == rect.y) {
            PageInfo *pageInfo = dm->GetPageInfo(dm->CurrentPageNo());
            scroll.y = -(pageInfo->pageOnScreen.y - dm->GetPadding()->top);
            scroll.y = max(scroll.y, 0); // Adobe Reader never shows the previous page
        }
    }
    else if (rect.dx != DEST_USE_DEFAULT && rect.dy != DEST_USE_DEFAULT) {
        // PDF: /FitR left bottom right top
        RectD rectD = dm->engine->Transform(rect, pageNo, dm->ZoomReal(), dm->Rotation());
        scroll = rectD.TL().Convert<int>();

        // Rect<float> rectF = dm->engine->Transform(rect, pageNo, 1.0, dm->rotation()).Convert<float>();
        // zoom = 100.0f * min(owner->canvasRc.dx / rectF.dx, owner->canvasRc.dy / rectF.dy);
    }
    else if (rect.y != DEST_USE_DEFAULT) {
        // PDF: /FitH top  or  /FitBH top
        PointD scrollD = dm->engine->Transform(rect.TL(), pageNo, dm->ZoomReal(), dm->Rotation());
        scroll.y = max(scrollD.Convert<int>().y, 0); // Adobe Reader never shows the previous page

        // zoom = FitBH ? ZOOM_FIT_CONTENT : ZOOM_FIT_WIDTH
    }
    // else if (Fit || FitV) zoom = ZOOM_FIT_PAGE
    // else if (FitB || FitBV) zoom = ZOOM_FIT_CONTENT
    /* // ignore author-set zoom settings (at least as long as there's no way to overrule them)
    if (zoom != INVALID_ZOOM) {
        // TODO: adjust the zoom level before calculating the scrolling coordinates
        dm->zoomTo(zoom);
        UpdateToolbarState(owner);
    }
    // */
    dm->GoToPage(pageNo, scroll.y, true, scroll.x);
}

void LinkHandler::LaunchFile(const TCHAR *path, PageDestination *link)
{
    // for safety, only handle relative paths and only open them in SumatraPDF
    // (unless they're of an allowed perceived type) and never launch any external
    // file in plugin mode (where documents are supposed to be self-contained)
    TCHAR drive;
    if (str::StartsWith(path, _T("\\")) || str::Parse(path, _T("%c:\\"), &drive) || gPluginMode) {
        return;
    }

    ScopedMem<TCHAR> fullPath(path::GetDir(owner->dm->FilePath()));
    fullPath.Set(path::Join(fullPath, path));
    fullPath.Set(path::Normalize(fullPath));
    // TODO: respect link->ld.gotor.new_window for PDF documents ?
    WindowInfo *newWin = FindWindowInfoByFile(fullPath);
    // TODO: don't show window until it's certain that there was no error
    if (!newWin) {
        LoadArgs args(fullPath, owner);
        newWin = LoadDocument(args);
        if (!newWin)
            return;
    }

    if (!newWin->IsDocLoaded()) {
        CloseWindow(newWin, true, false);
        // OpenFileExternally rejects files we'd otherwise
        // have to show a notification to be sure (which we
        // consider bad UI and thus simply don't)
        bool ok = OpenFileExternally(fullPath);
        if (!ok) {
            ScopedMem<TCHAR> msg(str::Format(_TR("Error loading %s"), fullPath));
            ShowNotification(owner, msg, true /* autoDismiss */, true /* highlight */);
        }
        return;
    }

    newWin->Focus();
    if (!link)
        return;

    ScopedMem<TCHAR> name(link->GetDestName());
    if (!name)
        newWin->linkHandler->ScrollTo(link);
    else {
        PageDestination *dest = newWin->dm->engine->GetNamedDest(name);
        if (dest) {
            newWin->linkHandler->ScrollTo(dest);
            delete dest;
        }
    }
}

// normalizes case and whitespace in the string
// caller needs to free() the result
static TCHAR *NormalizeFuzzy(const TCHAR *str)
{
    TCHAR *dup = str::Dup(str);
    CharLower(dup);
    str::NormalizeWS(dup);
    // cf. AddTocItemToView
    return dup;
}

static bool MatchFuzzy(const TCHAR *s1, const TCHAR *s2, bool partially=false)
{
    if (!partially)
        return str::Eq(s1, s2);

    // only match at the start of a word (at the beginning and after a space)
    for (const TCHAR *last = s1; (last = str::Find(last, s2)); last++)
        if (last == s1 || *(last - 1) == ' ')
            return true;
    return false;
}

// finds the first ToC entry that (partially) matches a given normalized name
// (ignoring case and whitespace differences)
PageDestination *LinkHandler::FindTocItem(DocTocItem *item, const TCHAR *name, bool partially)
{
    for (; item; item = item->next) {
        ScopedMem<TCHAR> fuzTitle(NormalizeFuzzy(item->title));
        if (MatchFuzzy(fuzTitle, name, partially))
            return item->GetLink();
        PageDestination *dest = FindTocItem(item->child, name, partially);
        if (dest)
            return dest;
    }
    return NULL;
}

void LinkHandler::GotoNamedDest(const TCHAR *name)
{
    assert(owner && owner->linkHandler == this);
    if (!engine())
        return;

    // Match order:
    // 1. Exact match on internal destination name
    // 2. Fuzzy match on full ToC item title
    // 3. Fuzzy match on a part of a ToC item title
    PageDestination *dest = engine()->GetNamedDest(name);
    if (dest) {
        ScrollTo(dest);
        delete dest;
    }
    else if (engine()->HasTocTree()) {
        DocTocItem *root = engine()->GetTocTree();
        ScopedMem<TCHAR> fuzName(NormalizeFuzzy(name));
        dest = FindTocItem(root, fuzName);
        if (!dest)
            dest = FindTocItem(root, fuzName, true);
        if (dest)
            ScrollTo(dest);
        delete root;
    }
}
