/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#ifndef Mui_h
#error "this is only meant to be included by Mui.h inside mui namespace"
#endif
#ifdef MuiCss_h
#error "dont include twice!"
#endif
#define MuiCss_h

namespace css {

enum PropType {
    PropFontName = 0,       // font-family
    PropFontSize,           // font-size
    PropFontWeight,         // font-weight
    PropPadding,            // padding
    PropColor,              // color
    PropBgColor,            // background-color

    PropBorderTopWidth,     // border-top-width
    PropBorderRightWidth,   // border-right-width
    PropBorderBottomWidth,  // border-bottom-width
    PropBorderLeftWidth,    // border-left-width

    PropBorderTopColor,     // border-top-color
    PropBorderRightColor,   // border-right-color
    PropBorderBottomColor,  // border-bottom-color
    PropBorderLeftColor,    // border-left-color

    PropTextAlign,          // text-align

    // used to define horizontal/vertical alignment of an element
    // inside a container. Used e.g. for a ButtonVector
    PropVertAlign,
    PropHorizAlign,

    PropFill,               // fill, used for svg::path
    PropStroke,             // stroke, used for svg::path
    PropStrokeWidth,        // stroke-width, used for svg::path

    PropsCount              // must be at the end!
};

bool IsWidthProp(PropType type);
bool IsColorProp(PropType type);
bool IsAlignProp(PropType type);

// Align is aname  common so to avoid potential conflicts, use ElAlign
// which stands for Element Align.
// Top/Left and Bottom/Right are represented by the same ElAlignData
// values but they're semantically different, so we given them unique names
enum ElAlign {
    ElAlignCenter,
    ElAlignTop,
    ElAlignBottom,
    ElAlignLeft,
    ElAlignRight
};

// A generalized way of specifying alignment (on a single axis,
// vertical or horizontal) of an element relative to its container.
// Each point of both the container and element can be represented
// as a float in the <0.f - 1.f> range.
// O.f represents left (in horizontal case) or top (in vertical) case point.
// 1.f represents right/bottom point and 0.5f represents a middle.
// We define a point inside cotainer and point inside element and layout
// positions element so that those points are the same.
// For example:
//  - (0.5f, 0.5f) centers element inside of the container.
//  - (0.f, 0.f) makes left edge of the element align with left edge of the container
//    i.e. ||el| container|
//  - (1.f, 0.f) makes left edge of the element align with right edge of the container
//    i.e. |container||el|
// This is more flexible than, say, VerticalAlignment property in WPF.
// Note: this can be extended for values outside of <0.f - 1.f> range.
struct ElAlignData {

    float elementPoint;
    float containerPoint;

    bool operator==(const ElAlignData& other) const;

    void Set(ElAlign align);
    int CalcOffset(int elSize, int containerSize);
};

// we can't have constructors in ElInContainerAlign, so those are
// helper methods for constructing them
static inline ElAlignData GetElAlignCenter() {
    ElAlignData align = { .5f, .5f };
    return align;
}

static inline ElAlignData GetElAlignTop() {
    ElAlignData align = { 0.f, 0.f };
    return align;
}

static inline ElAlignData GetElAlignLeft() {
    ElAlignData align = { 0.f, 0.f };
    return align;
}

static inline ElAlignData GetElAlignBottom() {
    ElAlignData align = { 1.f, 1.f };
    return align;
}

static inline ElAlignData GetElAlignRight() {
    ElAlignData align = { 1.f, 1.f };
    return align;
}

static inline ElAlignData GetElAlign(float ep, float cp) {
    ElAlignData align = { ep, cp };
    return align;
}

enum ColorType {
    ColorSolid,
    ColorGradientLinear,
    // TODO: other gradient types?
};

struct ColorDataSolid {
    ARGB    color;
    Brush * cachedBrush;
};

struct ColorDataGradientLinear {
    LinearGradientMode    mode;
    ARGB                  startColor;
    ARGB                  endColor;
    RectF *               rect;
    LinearGradientBrush * cachedBrush;
};

struct ColorData {
    ColorType   type;
    union {
        ColorDataSolid          solid;
        ColorDataGradientLinear gradientLinear;
    };

    bool operator==(const ColorData& other) const;
};

struct Padding {
    int top, right, bottom, left;
    bool operator ==(const Padding& other) const {
        return (top == other.top) &&
               (right == other.right) &&
               (bottom == other.bottom) &&
               (left == other.left);
    }
};

struct Prop {

    Prop(PropType type) : type(type) {}

    void Free();

    PropType    type;

    union {
        WCHAR *     fontName;
        float       fontSize;
        FontStyle   fontWeight;
        Padding     padding;
        ColorData   color;
        float       width;
        AlignAttr   textAlign;
        ElAlignData elAlign;
    };

    bool Eq(const Prop* other) const;

    static Prop *AllocFontName(const WCHAR *name);
    static Prop *AllocFontSize(float size);
    static Prop *AllocFontWeight(FontStyle style);
    // TODO: add AllocTextAlign(const char *s);
    static Prop *AllocTextAlign(AlignAttr align);
    static Prop *AllocAlign(PropType type, float elPoint, float containerPoint);
    static Prop *AllocAlign(PropType type, ElAlign align);
    static Prop *AllocPadding(int top, int right, int bottom, int left);
    static Prop *AllocColorSolid(PropType type, ARGB color);
    static Prop *AllocColorSolid(PropType type, int a, int r, int g, int b);
    static Prop *AllocColorSolid(PropType type, int r, int g, int b);
    static Prop *AllocColorSolid(PropType type, const char *color);
    static Prop *AllocColorLinearGradient(PropType type, LinearGradientMode mode, ARGB startColor, ARGB endColor);
    static Prop *AllocColorLinearGradient(PropType type, LinearGradientMode mode, const char *startColor, const char *endColor);
    static Prop *AllocWidth(PropType type, float width);
};

class Style {
    // if property is not found here, we'll search the
    // inheritance chain
    Style *     inheritsFrom;
    // generation number, changes every time we change the style
    size_t      gen;

public:
    Style(Style *inheritsFrom=NULL) : inheritsFrom(inheritsFrom) {
        gen = 1; // so that we can use 0 for NULL
    }

    Vec<Prop*>  props;

    void Set(Prop *prop);

    // shortcuts for setting multiple properties at a time
    void SetBorderWidth(float width);
    void SetBorderColor(ARGB color);

    Style * GetInheritsFrom() const;
    size_t GetIdentity() const;
};

struct BorderWidth {
    float top, right, bottom, left;
};

struct BorderColors {
    ColorData *top;
    ColorData *right;
    ColorData *bottom;
    ColorData *left;
};

// CachedStyle combines values of all properties for easier use by clients
struct CachedStyle {
    const WCHAR *   fontName;
    float           fontSize;
    FontStyle       fontWeight;
    Padding         padding;
    ColorData *     color;
    ColorData *     bgColor;
    BorderWidth     borderWidth;
    BorderColors    borderColors;
    AlignAttr       textAlign;
    ElAlignData     vertAlign;
    ElAlignData     horizAlign;
    ColorData *     fill;
    ColorData *     stroke;
    float           strokeWidth;
};

// globally known properties for elements we know about
// we fill them with default values and they can be
// modified by an app for global visual makeover
extern Style *gStyleDefault;
extern Style *gStyleButtonDefault;
extern Style *gStyleButtonMouseOver;

void   Initialize();
void   Destroy();

CachedStyle* CacheStyle(Style *style);

Brush *BrushFromColorData(ColorData *color, const Rect& r);
Brush *BrushFromColorData(ColorData *color, const RectF& r);

} // namespace css

