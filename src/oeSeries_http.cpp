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

#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/socket.h>

#include <string>

#include "oeSeries_http.h"
#include "oeSeries_log.h"

// Explicit, short timeout so an unreachable host cannot block for the ~21s OS
// default. Applies to connect and each blocking read/write. Off the main thread.
static const int OE_HTTP_TIMEOUT_SEC = 3;

HttpWorker::HttpWorker()
    : wxThread(wxTHREAD_JOINABLE),
      m_cond(m_mutex),
      m_started(false),
      m_exit(false),
      m_has_request(false),
      m_has_result(false),
      m_busy(false),
      m_req_tag(0),
      m_port(0)
{
}

HttpWorker::~HttpWorker()
{
    Shutdown();
}

bool HttpWorker::Start()
{
    if (m_started)
        return true;
    if (Create() != wxTHREAD_NO_ERROR)
        return false;
    if (Run() != wxTHREAD_NO_ERROR)
        return false;
    m_started = true;
    return true;
}

void HttpWorker::Shutdown()
{
    if (!m_started)
        return;
    {
        wxMutexLocker lock(m_mutex);
        m_exit = true;
        m_cond.Signal();
    }
    Wait();   // join (joinable thread)
    m_started = false;
}

bool HttpWorker::Submit(int tag, const wxString &method, const wxString &host,
                        int port, const wxString &path, const wxString &body)
{
    wxMutexLocker lock(m_mutex);
    if (m_busy || m_has_request)
        return false;
    m_req_tag = tag;
    m_method = method;
    m_host = host;
    m_port = port;
    m_path = path;
    m_body = body;
    m_has_request = true;
    m_busy = true;
    m_cond.Signal();
    return true;
}

bool HttpWorker::IsBusy()
{
    wxMutexLocker lock(m_mutex);
    return m_busy;
}

bool HttpWorker::TryGetResult(HttpResult &out)
{
    wxMutexLocker lock(m_mutex);
    if (!m_has_result)
        return false;
    out = m_result;
    m_has_result = false;
    return true;
}

wxThread::ExitCode HttpWorker::Entry()
{
    for (;;)
    {
        int tag, port;
        wxString method, host, path, body;
        {
            wxMutexLocker lock(m_mutex);
            while (!m_exit && !m_has_request)
                m_cond.Wait();
            if (m_exit)
                break;
            tag = m_req_tag;
            port = m_port;
            method = m_method;
            host = m_host;
            path = m_path;
            body = m_body;
            m_has_request = false;
        }

        HttpResult r = DoRequest(method, host, port, path, body, tag);

        {
            wxMutexLocker lock(m_mutex);
            m_result = r;
            m_has_result = true;
            m_busy = false;
        }
    }
    return (ExitCode)0;
}

// Write all bytes (wxSOCKET_BLOCK: blocks until sent or timeout).
static bool write_all(wxSocketBase &sock, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        sock.Write(data + off, (wxUint32)(len - off));
        size_t w = sock.LastWriteCount();
        if (w == 0)
            return false;   // error or timeout
        off += w;
    }
    return true;
}

HttpResult HttpWorker::DoRequest(const wxString &method, const wxString &host,
                                 int port, const wxString &path,
                                 const wxString &body, int tag)
{
    HttpResult r;
    r.tag = tag;

    wxIPV4address addr;
    if (!addr.Hostname(host))
    {
        r.error = "cannot resolve host " + host;
        return r;
    }
    addr.Service((unsigned short)port);

    wxSocketClient sock(wxSOCKET_BLOCK);
    sock.SetTimeout(OE_HTTP_TIMEOUT_SEC);
    if (!sock.Connect(addr, true) || !sock.IsConnected())
    {
        r.error = "connect refused/timeout";
        return r;
    }

    // Build the request. Connection: close lets us read until EOF.
    wxScopedCharBuffer bodybuf = body.ToUTF8();
    wxString head;
    head << method << " " << path << " HTTP/1.1\r\n";
    head << "Host: " << host << ":" << port << "\r\n";
    head << "User-Agent: oESeries\r\n";
    head << "Accept: application/json\r\n";
    head << "Connection: close\r\n";
    if (method == "POST")
    {
        head << "Content-Type: application/json\r\n";
        head << "Content-Length: " << (int)bodybuf.length() << "\r\n";
    }
    head << "\r\n";

    wxScopedCharBuffer headbuf = head.ToUTF8();
    if (!write_all(sock, headbuf.data(), headbuf.length()) ||
        (method == "POST" && bodybuf.length() > 0 &&
         !write_all(sock, bodybuf.data(), bodybuf.length())))
    {
        r.error = "write failed";
        sock.Close();
        return r;
    }

    // Read the whole response until the server closes the connection.
    std::string resp;
    char buf[2048];
    for (;;)
    {
        sock.Read(buf, sizeof(buf));
        size_t n = sock.LastReadCount();
        if (n > 0)
            resp.append(buf, n);
        if (n == 0)
            break;   // EOF (Connection: close) or timeout
    }
    sock.Close();

    if (resp.empty())
    {
        r.error = "empty response";
        return r;
    }

    // Parse status line and split headers/body.
    // Status line: "HTTP/1.1 200 OK"
    size_t sp1 = resp.find(' ');
    if (sp1 != std::string::npos)
        r.status = atoi(resp.c_str() + sp1 + 1);

    size_t hdr_end = resp.find("\r\n\r\n");
    std::string body_str;
    if (hdr_end != std::string::npos)
        body_str = resp.substr(hdr_end + 4);
    else
        body_str = resp;   // no header terminator seen; take it all

    r.body = wxString::FromUTF8(body_str.c_str(), body_str.size());
    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.IsEmpty())
        r.error = wxString::Format("HTTP status %d", r.status);
    return r;
}
