/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  oESeries Plugin - leveled logger
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

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/datetime.h>
#include <wx/thread.h>

#include <stdarg.h>
#include <stdio.h>

#include "oeSeries_log.h"
#include "ocpn_plugin.h"   // GetpPrivateApplicationDataLocation

// The live debug level, defined in oeSeries_pi.cpp and mirrored from the pref.
extern int g_oeseries_debug_level;

static wxMutex s_log_mutex;
static wxFile *s_log_file = nullptr;

static wxString oe_timestamp()
{
    wxDateTime now = wxDateTime::UNow();
    return now.Format("%Y-%m-%d %H:%M:%S.") +
           wxString::Format("%03d", (int)now.GetMillisecond());
}

void oeLogInit()
{
    wxMutexLocker lock(s_log_mutex);
    if (s_log_file)
        return;

    wxString dir;
    wxString *p = GetpPrivateApplicationDataLocation();
    if (p)
        dir = *p;
    if (dir.IsEmpty())
        return;

    wxFileName fn(dir, "oESeries.log");
    if (!wxFileName::DirExists(fn.GetPath()))
        wxFileName::Mkdir(fn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    s_log_file = new wxFile();
    if (!s_log_file->Open(fn.GetFullPath(), wxFile::write_append))
    {
        delete s_log_file;
        s_log_file = nullptr;
        return;
    }

    wxString banner;
    banner << "\n==== oESeries log opened " << oe_timestamp() << " ====\n";
    s_log_file->Write(banner);
    s_log_file->Flush();
}

void oeLogClose()
{
    wxMutexLocker lock(s_log_mutex);
    if (!s_log_file)
        return;
    if (s_log_file->IsOpened())
    {
        wxString banner;
        banner << "==== oESeries log closed " << oe_timestamp() << " ====\n";
        s_log_file->Write(banner);
        s_log_file->Flush();
        s_log_file->Close();
    }
    delete s_log_file;
    s_log_file = nullptr;
}

void oeLog(int level, int indent, const char *fmt, ...)
{
    if (level > g_oeseries_debug_level)
        return;

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';

    if (indent < 0)
        indent = 0;
    if (indent > 12)
        indent = 12;

    wxString line;
    line << oe_timestamp() << " ";
    for (int i = 0; i < indent; i++)
        line << "  ";
    line << wxString::FromUTF8(body) << "\n";

    wxMutexLocker lock(s_log_mutex);
    if (!s_log_file || !s_log_file->IsOpened())
        return;
    s_log_file->Write(line);
    s_log_file->Flush();
}
