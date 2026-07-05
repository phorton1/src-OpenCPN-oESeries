/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  oESeries Plugin - navMate spoke
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

#ifndef OESERIES_PI_H_
#define OESERIES_PI_H_

#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/fileconf.h>

#include "config.h"
#include "ocpn_plugin.h"   // Required for OCPN plugin functions

// Global debug level, mirrored from the preference. Messages are filtered by
// level (see the leveled logger). Kept as a file global so the worker/HTTP code
// can read it without a plugin pointer.
extern int g_oeseries_debug_level;

class HttpWorker;
class oeTimer;

//----------------------------------------------------------------------------
//    The PlugIn Class Definition
//----------------------------------------------------------------------------

// NOTE: the base class MUST match the API version reported by
// GetAPIVersionMajor/Minor (1.20, from config.h / the api-20 lib). OpenCPN's
// plugin_loader dispatches on the reported version and does
// dynamic_cast<opencpn_plugin_120*>(this) - if the object is not actually an
// opencpn_plugin_120, the cast yields NULL and the host logs "Incompatible
// plugin detected". So report-1.20 <-> derive-from-opencpn_plugin_120.
class oESeriesPi : public opencpn_plugin_120
{
public:
    explicit oESeriesPi(void *ppimgr);
    ~oESeriesPi() override;

    //    The required PlugIn Methods
    int Init() override;
    bool DeInit() override;

    int GetAPIVersionMajor() override;
    int GetAPIVersionMinor() override;
    int GetPlugInVersionMajor() override;
    int GetPlugInVersionMinor() override;
    int GetPlugInVersionPatch() override;
    int GetPlugInVersionPost() override;
    const char *GetPlugInVersionPre() override;
    const char *GetPlugInVersionBuild() override;

    wxBitmap *GetPlugInBitmap() override;
    wxString GetCommonName() override;
    wxString GetShortDescription() override;
    wxString GetLongDescription() override;

    //    Preferences
    void ShowPreferencesDialog(wxWindow *parent) override;

    //    Heartbeat - fired by oeTimer on the MAIN thread. Enumerates the model,
    //    hashes it, advances DT_ocpn on change, and drives the poll/POST loop.
    void OnTimer();

private:
    bool LoadConfig();
    bool SaveConfig();

    // Main-thread: enumerate OpenCPN's model, hash it, and (on change) advance
    // m_dt_ocpn and rebuild the cached inventory arrays (m_marks/routes/tracks_json).
    void EnumerateAndBuild();

    // Assemble the full POST body {dt, marks, routes, tracks, results} from the
    // cached inventory arrays + any pending results[] (sec 2A).
    wxString BuildPostBody();

    // M3: parse a GET view's commands[] (nlohmann), apply each on the main thread
    // (merge-on-apply, sec 8; diag channel), stash results[] for the next POST.
    void ApplyGetView(const wxString &body);

    wxFileConfig *m_config;
    wxWindow *m_parent_window;

    //    Persisted preferences
    wxString m_host_port;    // navMate "host:port" (dev default localhost:9883)
    int m_debug_level;       // verbosity, mirrored to g_oeseries_debug_level

    //    Sync engine
    oeTimer *m_timer;        // main-thread heartbeat
    HttpWorker *m_http;      // off-main-thread HTTP client

    unsigned long long m_dt_ocpn;    // my DT token (minted only by oESeries)
    unsigned long long m_last_hash;  // FNV-1a of the last enumerated inventory
    bool m_have_hash;                // false until the first enumeration
    int m_wp_count;                  // distinct marks in the last inventory
    int m_route_count;               // routes in the last inventory (diag)
    int m_track_count;               // tracks in the last inventory (diag)
    int m_vertices_seen;             // embedded route vertices last inventory (diag)

    // Cached inventory arrays (serialized JSON, ASCII), rebuilt on change; the
    // POST body is assembled from these + m_pending_results at send time.
    bool m_have_inventory;           // true after the first EnumerateAndBuild
    wxString m_marks_json;           // last marks[]  (e.g. "[{...},...]")
    wxString m_routes_json;          // last routes[]
    wxString m_tracks_json;          // last tracks[]

    bool m_want_post;                // last GET showed navMate behind -> POST
    long long m_navmate_dt;          // last navmate_dt seen (0 until db gate)
    bool m_synced;                   // navMate has acknowledged our current DT
    bool m_reachable;                // navMate answered our last request

    // M3 apply/echo state
    wxString m_pending_results;      // results[] to attach to the next POST ("" = none)
    long long m_last_applied_batch;  // count of applied command batches (diag state)
    long long m_echo_baseline_dt;    // DT_ocpn snapshot at last apply (echo marker)
    unsigned long long m_echo_baseline_hash;  // hash snapshot at last apply

    wxBitmap m_panel_bitmap;
};

#endif   // OESERIES_PI_H_
