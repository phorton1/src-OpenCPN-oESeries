/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  oESeries Plugin - off-main-thread HTTP client
 * Author:   Patrick Horton
 *
 ***************************************************************************
 *   Copyright (C) 2026 by Patrick Horton                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************
 */

#ifndef OESERIES_HTTP_H_
#define OESERIES_HTTP_H_

#include <wx/string.h>
#include <wx/thread.h>

// Result of one HTTP exchange, marshaled back to the main thread.
struct HttpResult
{
    int tag;         // caller tag to correlate the request (GET vs POST)
    bool ok;         // completed with a 2xx response
    int status;      // HTTP status code (0 if none)
    wxString body;   // response body
    wxString error;  // human-readable failure reason if !ok

    HttpResult() : tag(0), ok(false), status(0) {}
};

// A single persistent worker thread that performs one HTTP request at a time
// against navMate, off the main thread, with a short connect/IO timeout. The
// plugin's wxTimer (main thread) submits a request and, on a later tick, polls
// for the result - no wxEvtHandler plumbing, single-flight by construction.
class HttpWorker : public wxThread
{
public:
    HttpWorker();
    ~HttpWorker() override;

    bool Start();      // create + run the thread
    void Shutdown();   // signal exit and join

    // Main thread: submit a request. Returns false if one is already in flight.
    // method is "GET" or "POST"; body is the request body for POST.
    bool Submit(int tag, const wxString &method, const wxString &host, int port,
                const wxString &path, const wxString &body);

    bool IsBusy();                        // a request is in flight
    bool TryGetResult(HttpResult &out);   // take a completed result if ready

protected:
    ExitCode Entry() override;

private:
    HttpResult DoRequest(const wxString &method, const wxString &host, int port,
                         const wxString &path, const wxString &body, int tag);

    wxMutex m_mutex;
    wxCondition m_cond;   // signaled on submit or shutdown

    bool m_started;
    bool m_exit;
    bool m_has_request;
    bool m_has_result;
    bool m_busy;

    // pending request
    int m_req_tag;
    int m_port;
    wxString m_method;
    wxString m_host;
    wxString m_path;
    wxString m_body;

    // completed result
    HttpResult m_result;
};

#endif   // OESERIES_HTTP_H_
