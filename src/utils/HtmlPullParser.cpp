/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"
#include "HtmlPullParser.h"

/* TODO: We could extend the parser to allow navigating the tree (go to a prev/next sibling, go
to a parent) without explicitly building a tree in memory. I think all we need to do is to extend
tagNesting to also remember the position in html of the tag. Given this information we should be
able to navigate the tree by reparsing.*/

// returns -1 if didn't find
int HtmlEntityNameToRune(const char *name, size_t nameLen)
{
    return FindHtmlEntityRune(name, nameLen);
}

#define MAX_ENTITY_NAME_LEN 8

// A unicode version of HtmlEntityNameToRune. It's safe because 
// entity names only contain ascii (<127) characters so if a simplistic
// conversion from unicode to ascii succeeds, we can use ascii
// version, otherwise it wouldn't match anyway
// returns -1 if didn't find
int HtmlEntityNameToRune(const WCHAR *name, size_t nameLen)
{
    char asciiName[MAX_ENTITY_NAME_LEN];
    if (nameLen > MAX_ENTITY_NAME_LEN)
        return -1;
    char *s = asciiName;
    for (size_t i = 0; i < nameLen; i++) {
        if (name[i] > 127)
            return -1;
        asciiName[i] = (char)name[i];
    }
    return FindHtmlEntityRune(asciiName, nameLen);
}

static bool SkipUntil(const char*& s, const char *end, char c)
{
    while ((s < end) && (*s != c)) {
        ++s;
    }
    return *s == c;
}

static bool SkipUntil(const char*& s, const char *end, char *term)
{
    size_t len = str::Len(term);
    for (; s < end; s++) {
        if (s + len < end && str::StartsWith(s, term))
            return true;
    }
    return false;
}

// return true if skipped
bool SkipWs(const char* & s, const char *end)
{
    const char *start = s;
    while ((s < end) && str::IsWs(*s)) {
        ++s;
    }
    return start != s;
}

// return true if skipped
bool SkipNonWs(const char* & s, const char *end)
{
    const char *start = s;
    while ((s < end) && !str::IsWs(*s)) {
        ++s;
    }
    return start != s;
}

static int IsNameChar(char c)
{
    return c == '.' || c == '-' || c == '_' || c == ':' ||
           str::IsDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// skip all html tag or attribute characters
static void SkipName(const char*& s, const char *end)
{
    while ((s < end) && IsNameChar(*s)) {
        s++;
    }
}

// return true if s consists only of whitespace
bool IsSpaceOnly(const char *s, const char *end)
{
    SkipWs(s, end);
    return s == end;
}

void MemAppend(char *& dst, const char *s, size_t len)
{
    if (0 == len)
        return;
    memcpy(dst, s, len);
    dst += len;
}

// if "&foo;" was the entity, s points at the char
// after '&' and len is the maximum lenght of the string
// (4 in case of "foo;")
// returns a pointer to the first character after the entity
static const char *ResolveHtmlEntity(const char *s, size_t len, int& rune)
{
    const char *entEnd = str::Parse(s, len, "#%d%?;", &rune);
    if (entEnd)
        return entEnd;
    entEnd = str::Parse(s, len, "#x%x%?;", &rune);
    if (entEnd)
        return entEnd;

    // go to the end of a potential named entity
    for (entEnd = s; entEnd < s + len && isalnum(*entEnd); entEnd++);
    if (entEnd != s) {
        rune = HtmlEntityNameToRune(s, entEnd - s);
        if (-1 == rune)
            return NULL;
        // skip the trailing colon - if there is one
        if (entEnd < s + len && *entEnd == ';')
            entEnd++;
        return entEnd;
    }

    rune = -1;
    return NULL;
}

// if s doesn't contain html entities, we just return it
// if it contains html entities, we'll return string allocated
// with alloc in which entities are converted to their values
// Entities are encoded as utf8 in the result.
// alloc can be NULL, in which case we'll allocate with malloc()
const char *ResolveHtmlEntities(const char *s, const char *end, Allocator *alloc)
{
    char *        res = NULL;
    size_t        resLen = 0;
    char *        dst;

    const char *curr = s;
    for (;;) {
        bool found = SkipUntil(curr, end, '&');
        if (!found) {
            if (!res)
                return s;
            // copy the remaining string
            MemAppend(dst, s, end - s);
            break;
        }
        if (!res) {
            // allocate memory for the result string
            // I'm banking that text after resolving entities will
            // be smaller than the original
            resLen = end - s + 8; // +8 just in case
            res = (char*)Allocator::Alloc(alloc, resLen);
            dst = res;
        }
        MemAppend(dst, s, curr - s);
        // curr points at '&'
        int rune = -1;
        const char *entEnd = ResolveHtmlEntity(curr + 1, end - curr - 1, rune);
        if (!entEnd) {
            // unknown entity, just copy the '&'
            MemAppend(dst, curr, 1);
            curr++;
        } else {
            str::Utf8Encode(dst, rune);
            curr = entEnd;
        }
        s = curr;
    }
    *dst = 0;
    CrashIf(dst >= res + resLen);
    return (const char*)res;
}

// convenience function for the above that always allocates
char *ResolveHtmlEntities(const char *s, size_t len)
{
    const char *tmp = ResolveHtmlEntities(s, s + len, NULL);
    if (tmp == s)
        return str::DupN(s, len);
    return (char *)tmp;
}

inline bool StrEqNIx(const char *s, size_t len, const char *s2)
{
    return str::Len(s2) == len && str::StartsWithI(s, s2);
}

bool AttrInfo::NameIs(const char *s) const
{
    return StrEqNIx(name, nameLen, s);
}

bool AttrInfo::ValIs(const char *s) const
{
    return StrEqNIx(val, valLen, s);
}

void HtmlToken::SetTag(TokenType new_type, const char *new_s, const char *end)
{
    type = new_type;
    s = new_s;
    sLen = end - s;
    SkipName(new_s, s + sLen);
    nLen = new_s - s;
    tag = FindHtmlTag(s, nLen);
    nextAttr = NULL;
}

void HtmlToken::SetText(const char *new_s, const char *end)
{
    type = Text;
    s = new_s;
    sLen = end - s;
}

void HtmlToken::SetError(ParsingError err, const char *errContext)
{
    type = Error;
    error = err;
    s = errContext;
}

bool HtmlToken::NameIs(const char *name) const
{
    return (str::Len(name) == nLen) && str::StartsWithI(s, name);
}

// reparse point is an address within html that we can
// can feed to HtmlPullParser() to start parsing from that point
const char *HtmlToken::GetReparsePoint() const
{
    if (IsStartTag() || IsEmptyElementEndTag())
        return s - 1;
    if (IsEndTag())
        return s - 2;
    if (IsText())
        return s;
    CrashIf(true); // don't call us on error tokens
    return NULL;
}

AttrInfo *HtmlToken::GetAttrByName(const char *name)
{
    nextAttr = NULL; // start from the beginning
    for (AttrInfo *a = NextAttr(); a; a = NextAttr()) {
        if (a->NameIs(name))
            return a;
    }
    return NULL;
}

AttrInfo *HtmlToken::GetAttrByValue(const char *name)
{
    size_t len = str::Len(name);
    nextAttr = NULL; // start from the beginning
    for (AttrInfo *a = NextAttr(); a; a = NextAttr()) {
        if (len == a->valLen && str::EqN(a->val, name, len))
            return a;
    }
    return NULL;
}

// We expect:
// whitespace | attribute name | = | attribute value
// where attribute value can be quoted
AttrInfo *HtmlToken::NextAttr()
{
    // start after the last attribute found (or the beginning)
    const char *curr = nextAttr;
    if (!curr)
        curr = s + nLen;
    const char *end = s + sLen;

    // parse attribute name
    SkipWs(curr, end);
    if (curr == end) {
NoNextAttr:
        nextAttr = NULL;
        return NULL;
    }
    attrInfo.name = curr;
    SkipName(curr, end);
    attrInfo.nameLen = curr - attrInfo.name;
    if (0 == attrInfo.nameLen)
        goto NoNextAttr;
    SkipWs(curr, end);
    if ((curr == end) || ('=' != *curr)) {
        // attributes without values get their names as value in HTML
        attrInfo.val = attrInfo.name;
        attrInfo.valLen = attrInfo.nameLen;
        nextAttr = curr;
        return &attrInfo;
    }

    // parse attribute value
    ++curr; // skip '='
    SkipWs(curr, end);
    if (curr == end) {
        // attribute with implicit empty value
        attrInfo.val = curr;
        attrInfo.valLen = 0;
    } else if (('\'' == *curr) || ('\"' == *curr)) {
        // attribute with quoted value
        ++curr;
        attrInfo.val = curr;
        if (!SkipUntil(curr, end, *(curr - 1)))
            goto NoNextAttr;
        attrInfo.valLen = curr - attrInfo.val;
        ++curr;
    } else {
        attrInfo.val = curr;
        SkipNonWs(curr, end);
        attrInfo.valLen = curr - attrInfo.val;
    }
    nextAttr = curr;
    return &attrInfo;
}

// Given a tag like e.g.:
// <tag attr=">" />
// tries to find the closing '>' and not be confused by '>' that
// are part of attribute value. We're not very strict here
// Returns false if didn't find 
static bool SkipUntilTagEnd(const char*& s, const char *end)
{
    while (s < end) {
        char c = *s++;
        if ('>' == c) {
            --s;
            return true;
        }
        if (('\'' == c) || ('"' == c)) {
            if (!SkipUntil(s, end, c))
                return false;
            ++s;
        }
    }
    return false;
}

// record the tag for the purpose of building current state
// of html tree
static void RecordStartTag(Vec<HtmlTag>* tagNesting, HtmlTag tag)
{
    if (!IsTagSelfClosing(tag))
        tagNesting->Append(tag);
}

// remove the tag from state of html tree
static void RecordEndTag(Vec<HtmlTag> *tagNesting, HtmlTag tag)
{
    // when closing a tag, if the top tag doesn't match but
    // there are only potentially self-closing tags on the
    // stack between the matching tag, we pop all of them
    if (tagNesting->Find(tag) != -1) {
        while ((tagNesting->Count() > 0) && (tagNesting->Last() != tag))
            tagNesting->Pop();
    }
    if (tagNesting->Count() > 0 && tagNesting->Last() == tag) {
        tagNesting->Pop();
    }
}

static void UpdateTagNesting(Vec<HtmlTag> *tagNesting, HtmlToken *t)
{
    // update the current state of html tree
    if (t->IsStartTag())
        RecordStartTag(tagNesting, t->tag);
    else if (t->IsEndTag())
        RecordEndTag(tagNesting, t->tag);
}

// Returns next part of html or NULL if finished
HtmlToken *HtmlPullParser::Next()
{
    if (currPos >= end)
        return NULL;

Next:
    const char *start = currPos;
    if (*currPos != '<') {
        // this must text between tags
        if (!SkipUntil(currPos, end, '<') && IsSpaceOnly(start, currPos)) {
            // ignore whitespace after the last tag
            return NULL;
        }
        currToken.SetText(start, currPos);
        return &currToken;
    }

    // '<' - tag begins
    ++start;

    // skip <? and <! (processing instructions and comments)
    if (('?' == *start) || ('!' == *start)) {
        if ('!' == *start && start + 2 < end && str::StartsWith(start, "!--")) {
            currPos = start + 2;
            if (!SkipUntil(currPos, end, "-->")) {
                currToken.SetError(HtmlToken::UnclosedTag, start);
                return &currToken;
            }
            currPos += 2;
        }
        else if (!SkipUntil(currPos, end, '>')) {
            currToken.SetError(HtmlToken::UnclosedTag, start);
            return &currToken;
        }
        ++currPos;
        goto Next;
    }

    if (!SkipUntilTagEnd(currPos, end)) {
        currToken.SetError(HtmlToken::UnclosedTag, start);
        return &currToken;
    }

    CrashIf('>' != *currPos);
    if (currPos == start || currPos == start + 1 && *start == '/') {
        // skip empty tags (<> and </>), because we're lenient
        ++currPos;
        goto Next;
    }

    if (('/' == *start) && ('/' == currPos[-1])) { // </foo/>
        currToken.SetError(HtmlToken::InvalidTag, start);
    } else if ('/' == *start) { // </foo>
        currToken.SetTag(HtmlToken::EndTag, start + 1, currPos);
    } else if ('/' == currPos[-1]) { // <foo/>
        currToken.SetTag(HtmlToken::EmptyElementTag, start, currPos - 1);
    } else {
        currToken.SetTag(HtmlToken::StartTag, start, currPos);
    }
    ++currPos;
    UpdateTagNesting(&tagNesting, &currToken);
    return &currToken;
}

// note: this is a bit unorthodox but allows us to easily separate
// (often long) unit tests into a separate file without additional
// makefile gymnastics
#ifdef DEBUG
#include "HtmlPullParser_ut.cpp"
#endif
