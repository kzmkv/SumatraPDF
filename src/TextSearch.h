/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef TextSearch_h
#define TextSearch_h

#include <windows.h>
#include "TextSelection.h"

enum TextSearchDirection {
    FIND_BACKWARD = false,
    FIND_FORWARD  = true
};

class ProgressUpdateUI
{
public:
    virtual void UpdateProgress(int current, int total) = 0;
    virtual bool WasCanceled() = 0;
};

class TextSearch : public TextSelection
{
public:
    TextSearch(BaseEngine *engine, PageTextCache *textCache);
    ~TextSearch();

    void SetSensitive(bool sensitive);
    void SetDirection(TextSearchDirection direction);
    TextSel *FindFirst(int page, TCHAR *text, ProgressUpdateUI *tracker=NULL);
    TextSel *FindNext(ProgressUpdateUI *tracker=NULL);

    // note: the result might not be a valid page number!
    int GetCurrentPageNo() const { return findPage; }

protected:
    TCHAR *findText;
    TCHAR *anchor;
    int findPage;
    bool forward;
    bool caseSensitive;
    // these two options are implicitly set when the search text begins
    // resp. ends in a single space (many users already search that way),
    // combining them yields a 'Whole words' search
    bool matchWordStart;
    bool matchWordEnd;

    void SetText(TCHAR *text);
    bool FindTextInPage(int pageNo = 0);
    bool FindStartingAtPage(int pageNo, ProgressUpdateUI *tracker);
    int MatchLen(const TCHAR *start);

    void Clear()
    {
        str::ReplacePtr(&findText, NULL);
        str::ReplacePtr(&anchor, NULL);
        str::ReplacePtr(&lastText, NULL);
        Reset();
    }
    void Reset();

private:
    const TCHAR *pageText;
    int findIndex;

    TCHAR *lastText;
    BYTE *findCache;
};

#endif
