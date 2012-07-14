/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef PsEngine_h
#define PsEngine_h

#include "BaseEngine.h"

class PsEngine : public BaseEngine {
public:
    virtual unsigned char *GetPDFData(size_t *cbCount) = 0;

public:
    static bool IsAvailable();
    static bool IsSupportedFile(const TCHAR *fileName, bool sniff=false);
    static PsEngine *CreateFromFile(const TCHAR *fileName);
};

#endif
