/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// note: include BaseUtil.h instead of including directly

template <typename T>
class PointT
{
public:
    T x, y;

    PointT() : x(0), y(0) { }
    PointT(T x, T y) : x(x), y(y) { }

    template <typename S>
    PointT<S> Convert() const {
        return PointT<S>((S)x, (S)y);
    }
    template <>
    PointT<int> Convert() const {
        return PointT<int>((int)floor(x + 0.5), (int)floor(y + 0.5));
    }

    bool operator==(PointT<T>& other) {
        return this->x == other.x && this->y == other.y;
    }
    bool operator!=(PointT<T>& other) {
        return !this->operator==(other);
    }
};

typedef PointT<int> PointI;
typedef PointT<double> PointD;

template <typename T>
class SizeT
{
public :
    T dx, dy;

    SizeT() : dx(0), dy(0) { }
    SizeT(T dx, T dy) : dx(dx), dy(dy) { }

    template <typename S>
    SizeT<S> Convert() const {
        return SizeT<S>((S)dx, (S)dy);
    }
    template <>
    SizeT<int> Convert() const {
        return SizeT<int>((int)floor(dx + 0.5), (int)floor(dy + 0.5));
    }

    bool operator==(SizeT<T>& other) {
        return this->dx == other.dx && this->dy == other.dy;
    }
    bool operator!=(SizeT<T>& other) {
        return !this->operator==(other);
    }
};

typedef SizeT<int> SizeI;
typedef SizeT<double> SizeD;

template <typename T>
class RectT
{
public:
    T x, y;
    T dx, dy;

    RectT() : x(0), y(0), dx(0), dy(0) { }
    RectT(T x, T y, T dx, T dy) : x(x), y(y), dx(dx), dy(dy) { }
    RectT(PointT<T> pt, SizeT<T> size) : x(pt.x), y(pt.y), dx(size.dx), dy(size.dy) { }

    static RectT FromXY(T xs, T ys, T xe, T ye) {
        if (xs > xe)
            swap(xs, xe);
        if (ys > ye)
            swap(ys, ye);
        return RectT(xs, ys, xe - xs, ye - ys);
    }
    static RectT FromXY(PointT<T> TL, PointT<T> BR) {
        return FromXY(TL.x, TL.y, BR.x, BR.y);
    }

    template <typename S>
    RectT<S> Convert() const {
        return RectT<S>((S)x, (S)y, (S)dx, (S)dy);
    }
    template <>
    RectT<int> Convert() const {
        return RectT<int>((int)floor(x + 0.5), (int)floor(y + 0.5),
                          (int)floor(dx + 0.5), (int)floor(dy + 0.5));
    }
    // cf. fz_roundrect in mupdf/fitz/base_geometry.c
#ifndef FLT_EPSILON
#define FLT_EPSILON 1.192092896e-07f
#endif
    RectT<int> Round() const {
        return RectT<int>::FromXY((int)floor(x + FLT_EPSILON),
                                  (int)floor(y + FLT_EPSILON),
                                  (int)ceil(x + dx - FLT_EPSILON),
                                  (int)ceil(y + dy - FLT_EPSILON));
    }

    bool IsEmpty() const {
        return dx == 0 || dy == 0;
    }

    bool Contains(PointT<T> pt) const {
        if (pt.x < this->x)
            return false;
        if (pt.x > this->x + this->dx)
            return false;
        if (pt.y < this->y)
            return false;
        if (pt.y > this->y + this->dy)
            return false;
        return true;
    }

    /* Returns an empty rectangle if there's no intersection (see IsEmpty). */
    RectT Intersect(RectT other) const {
        /* The intersection starts with the larger of the start
           coordinates and ends with the smaller of the end coordinates */
        T x = max(this->x, other.x);
        T dx = min(this->x + this->dx, other.x + other.dx) - x;
        T y = max(this->y, other.y);
        T dy = min(this->y + this->dy, other.y + other.dy) - y;

        /* return an empty rectangle if the dimensions aren't positive */
        if (dx <= 0 || dy <= 0)
            return RectT();
        return RectT(x, y, dx, dy);
    }

    RectT Union(RectT other) const {
        if (this->dx <= 0 && this->dy <= 0)
            return other;
        if (other.dx <= 0 && other.dy <= 0)
            return *this;

        T x = min(this->x, other.x);
        T y = min(this->y, other.y);
        T dx = max(this->x + this->dx, other.x + other.dx) - x;
        T dy = max(this->y + this->dy, other.y + other.dy) - y;

        return RectT(x, y, dx, dy);
    }

    void Offset(T _x, T _y) {
        x += _x;
        y += _y;
    }

    void Inflate(T _x, T _y) {
        x -= _x; dx += 2 * _x;
        y -= _y; dy += 2 * _y;
    }

    PointT<T> TL() const { return PointT<T>(x, y); }
    PointT<T> BR() const { return PointT<T>(x + dx, y + dy); }
    SizeT<T> Size() const { return SizeT<T>(dx, dy); }

#ifdef _WIN32
    RECT ToRECT() const {
        RectT<int> rectI(this->Convert<int>());
        RECT result = { rectI.x, rectI.y, rectI.x + rectI.dx, rectI.y + rectI.dy };
        return result;
    }
    static RectT FromRECT(RECT& rect) {
        return FromXY(rect.left, rect.top, rect.right, rect.bottom);
    }
#endif

    bool operator==(RectT<T>& other) {
        return this->x == other.x && this->y == other.y &&
               this->dx == other.dx && this->dy == other.dy;
    }
    bool operator!=(RectT<T>& other) {
        return !this->operator==(other);
    }
};

typedef RectT<int> RectI;
typedef RectT<double> RectD;

#ifdef _WIN32

class ClientRect : public RectI {
public:
    ClientRect(HWND hwnd) {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            x = rc.left; dx = rc.right - rc.left;
            y = rc.top; dy = rc.bottom - rc.top;
        }
    }
};

class WindowRect : public RectI {
public:
    WindowRect(HWND hwnd) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            x = rc.left; dx = rc.right - rc.left;
            y = rc.top; dy = rc.bottom - rc.top;
        }
    }
};

inline RectI MapRectToWindow(RectI rect, HWND hwndFrom, HWND hwndTo)
{
    RECT rc = rect.ToRECT();
    MapWindowPoints(hwndFrom, hwndTo, (LPPOINT)&rc, 2);
    return RectI::FromRECT(rc);
}

#endif

