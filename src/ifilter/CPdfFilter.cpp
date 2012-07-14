/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include "WinUtil.h"
#include "CPdfFilter.h"
#include "PdfEngine.h"

extern HINSTANCE g_hInstance;

VOID CPdfFilter::CleanUp()
{
    if (m_pdfEngine) {
        delete m_pdfEngine;
        m_pdfEngine = NULL;
    }
    m_state = STATE_PDF_END;
}

HRESULT CPdfFilter::OnInit()
{
    CleanUp();

    // TODO: PdfEngine::CreateFromStream never returns with
    //       m_pStream instead of a clone - why?

    // load content of PDF document into a seekable stream
    HRESULT res;
    size_t len;
    void *data = GetDataFromStream(m_pStream, &len, &res);
    if (!data)
        return res;

    ScopedComPtr<IStream> stream(CreateStreamFromData(data, len));
    free(data);
    if (!stream)
        return E_FAIL;

    m_pdfEngine = PdfEngine::CreateFromStream(stream);
    if (!m_pdfEngine)
        return E_FAIL;

    m_state = STATE_PDF_START;
    m_iPageNo = 0;
    return S_OK;
}

// copied from SumatraProperties.cpp
static bool PdfDateParse(const TCHAR *pdfDate, SYSTEMTIME *timeOut)
{
    ZeroMemory(timeOut, sizeof(SYSTEMTIME));
    // "D:" at the beginning is optional
    if (str::StartsWith(pdfDate, _T("D:")))
        pdfDate += 2;
    return str::Parse(pdfDate, _T("%4d%2d%2d") _T("%2d%2d%2d"),
        &timeOut->wYear, &timeOut->wMonth, &timeOut->wDay,
        &timeOut->wHour, &timeOut->wMinute, &timeOut->wSecond) != NULL;
    // don't bother about the day of week, we won't display it anyway
}

HRESULT CPdfFilter::GetNextChunkValue(CChunkValue &chunkValue)
{
    ScopedMem<TCHAR> str;

    switch (m_state) {
    case STATE_PDF_START:
        m_state = STATE_PDF_AUTHOR;
        chunkValue.SetTextValue(PKEY_PerceivedType, L"document");
        return S_OK;

    case STATE_PDF_AUTHOR:
        m_state = STATE_PDF_TITLE;
        str.Set(m_pdfEngine->GetProperty("Author"));
        if (!str::IsEmpty(str.Get())) {
            chunkValue.SetTextValue(PKEY_Author, AsWStrQ(str));
            return S_OK;
        }
        // fall through

    case STATE_PDF_TITLE:
        m_state = STATE_PDF_DATE;
        str.Set(m_pdfEngine->GetProperty("Title"));
        if (!str) str.Set(m_pdfEngine->GetProperty("Subject"));
        if (!str::IsEmpty(str.Get())) {
            chunkValue.SetTextValue(PKEY_Title, AsWStrQ(str));
            return S_OK;
        }
        // fall through

    case STATE_PDF_DATE:
        m_state = STATE_PDF_CONTENT;
        str.Set(m_pdfEngine->GetProperty("ModDate"));
        if (!str) str.Set(m_pdfEngine->GetProperty("CreationDate"));
        if (!str::IsEmpty(str.Get())) {
            SYSTEMTIME systime;
            if (PdfDateParse(str, &systime)) {
                FILETIME filetime;
                SystemTimeToFileTime(&systime, &filetime);
                chunkValue.SetFileTimeValue(PKEY_ItemDate, filetime);
                return S_OK;
            }
        }
        // fall through

    case STATE_PDF_CONTENT:
        while (++m_iPageNo <= m_pdfEngine->PageCount()) {
            str.Set(m_pdfEngine->ExtractPageText(m_iPageNo, _T("\r\n")));
            if (str::IsEmpty(str.Get()))
                continue;
            chunkValue.SetTextValue(PKEY_Search_Contents, AsWStrQ(str), CHUNK_TEXT);
            return S_OK;
        }
        m_state = STATE_PDF_END;
        // fall through

    case STATE_PDF_END:
    default:
        return FILTER_E_END_OF_CHUNKS;
    }
}
