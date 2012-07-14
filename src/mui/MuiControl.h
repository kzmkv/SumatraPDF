/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Must be included after Layout.h
#ifndef Mui_h
#error "this is only meant to be included by Mui.h inside mui namespace"
#endif
#ifdef MuiControl_h
#error "dont include twice!"
#endif
#define MuiControl_h

class EventMgr;

class Control : public ILayout
{
public:
    // allows a control to opt-out from being notified about
    // input events, stored in wantedInputBits
    enum WantedInputBits : int {
        WantsMouseOverBit   = 0,
        WantsMouseDownBit   = 1,
        WantsMouseUpBit     = 2,
        WantsMouseClickBit  = 3,
        WantsMouseMoveBit   = 4,
        WantedInputBitLast
    };

    // describes current state of a window, stored in stateBits
    enum ControlStateBits : int {
        MouseOverBit = 0,
        IsPressedBit = 1,
        // using IsHidden and not IsVisible so that 0 is default, visible state
        IsHiddenBit  = 2,
        StateBitLast
    };

    Control(Control *newParent=NULL);
    virtual ~Control();

    void        SetParent(Control *newParent);
    void        AddChild(Control *c, int pos = -1);
    void        AddChild(Control *c1, Control *c2, Control *c3 = NULL);
    Control *   GetChild(size_t idx) const;
    size_t      GetChildCount() const;

    void        SetPosition(const Rect& p);

    virtual void Paint(Graphics *gfx, int offX, int offY);

    // ILayout
    virtual void Measure(const Size availableSize);
    virtual void Arrange(const Rect finalRect);
    virtual Size DesiredSize();

    // mouse enter/leave are used e.g. by a button to change the look when mouse
    // is over them. The intention is that in response to those a window should
    // only do minimal processing that affects the window itself, not the rest
    // of the system
    virtual void NotifyMouseEnter() {}
    virtual void NotifyMouseLeave() {}

    virtual void NotifyMouseMove(int x, int y) {}

    virtual void RegisterEventHandlers(EventMgr *evtMgr) {}
    virtual void UnRegisterEventHandlers(EventMgr *evtMgr) {}

    bool WantsMouseClick() const;
    bool WantsMouseMove() const;
    bool IsMouseOver() const;
    void SetIsMouseOver(bool isOver);

    bool IsVisible() const;
    void Hide();
    void Show();

    void MeasureChildren(Size availableSize) const;
    void MapRootToMyPos(int& x, int& y) const;

    uint16          wantedInputBits; // WndWantedInputBits
    uint16          stateBits;       // WndStateBits
    // windows with bigger z-order are painted on top, 0 is default
    int16           zOrder;

    ILayout *       layout;
    Control *       parent;

    // we cache properties for the current style during SetStyle() which
    // makes if fast to access them anywhere without repeating the work
    // of searching the style inheritance chain
    CachedStyle *   cachedStyle;
    void            SetStyle(Style *style);

    // only used by HwndWrapper but we need it here
    HWND            hwndParent;

    // cursor to show when mouse is over this window.
    // only works if the window sets WantsMouseOverBit.
    // Control doesn't own hCursor in order to enable easy
    // sharing of cursor among many windows.
    HCURSOR         hCursor;

    // position and size (relative to parent, might be outside of parent's bounds)
    Rect            pos;

protected:
    Vec<Control*>   children;

    // desired size calculated in Measure()
    Size            desiredSize;
};

