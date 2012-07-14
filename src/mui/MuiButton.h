/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#ifndef Mui_h
#error "this is only meant to be included by Mui.h inside mui namespace"
#endif
#ifdef MuiButton_h
#error "dont include twice!"
#endif
#define MuiButton_h

class Button : public Control
{
    // use SetStyles() to set
    Style *         styleDefault;    // gStyleButtonDefault if styleDefault is NULL
    Style *         styleMouseOver;  // gStyleButtonMouseOver if NULL

public:
    Button(const WCHAR *s, Style *def, Style *mouseOver);

    virtual ~Button();

    void SetText(const WCHAR *s);

    void RecalculateSize(bool repaintIfSizeDidntChange);

    virtual void Measure(const Size availableSize);
    virtual void Paint(Graphics *gfx, int offX, int offY);

    virtual void NotifyMouseEnter();
    virtual void NotifyMouseLeave();

    void    SetStyles(Style *def, Style *mouseOver);

    WCHAR *         text;
    size_t          textDx; // cached measured text width
};

// TODO: maybe should combine Button and ButtonVector into one
class ButtonVector : public Control
{
    // use SetStyles() to set
    Style *         styleDefault;    // gStyleButtonDefault if styleDefault is NULL
    Style *         styleMouseOver;  // gStyleButtonMouseOver if NULL

    GraphicsPath *  graphicsPath;

public:
    ButtonVector(GraphicsPath *gp);

    virtual ~ButtonVector();

    void SetGraphicsPath(GraphicsPath *gp);

    void RecalculateSize(bool repaintIfSizeDidntChange);

    virtual void Measure(const Size availableSize);
    virtual void Paint(Graphics *gfx, int offX, int offY);

    virtual void NotifyMouseEnter();
    virtual void NotifyMouseLeave();

    void    SetStyles(Style *def, Style *mouseOver);
};

