/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD */

#ifndef CrashHandler_h
#define CrashHandler_h

void InstallCrashHandler(const TCHAR *crashDumpPath, const TCHAR *symDir);
void SubmitCrashInfo();
void UninstallCrashHandler();
void CrashLogFmt(const char *fmt, ...);

#endif
