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

#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/spinctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/filename.h>

#include <algorithm>
#include <string>
#include <vector>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include "config.h"
#include "oeSeries_pi.h"
#include "oeSeries_log.h"
#include "oeSeries_http.h"
#include "ocpn_plugin.h"

// Preference keys / defaults
static const char *const CONFIG_PATH = "/PlugIns/oESeries";
// Dev default: navMate runs its dev navServer on 9883 (packaged is 9873).
static const char *const DEFAULT_HOST_PORT = "localhost:9883";
static const long DEFAULT_DEBUG_LEVEL = 0;

// Heartbeat cadence and the endpoint. One request per tick, single-flight.
static const int POLL_INTERVAL_MS = 2000;
static const char *const OCPN_API_PATH = "/api/ocpn";
static const int TAG_GET = 1;
static const int TAG_POST = 2;

// Definition of the global declared in oeSeries_pi.h
int g_oeseries_debug_level = 0;

//----------------------------------------------------------------------------
//    Main-thread heartbeat timer. wxTimer::Notify() fires on the main thread;
//    subclassing avoids needing the plugin to be a wxEvtHandler.
//----------------------------------------------------------------------------

class oeTimer : public wxTimer
{
public:
    explicit oeTimer(oESeriesPi *pi) : m_pi(pi) {}
    void Notify() override { m_pi->OnTimer(); }

private:
    oESeriesPi *m_pi;
};

//----------------------------------------------------------------------------
//    Small helpers: DT formatting, host:port parse, JSON build + minimal parse.
//----------------------------------------------------------------------------

// A snapshot of one waypoint's sync-relevant fields.
struct WpEntry
{
    wxString guid;
    wxString name;
    wxString desc;
    wxString icon;
    double lat;
    double lon;
    bool visible;
};

// Unsigned-64 to string (avoids wxString::Format %llu portability worries).
static wxString DtStr(unsigned long long v)
{
    char b[32];
    snprintf(b, sizeof(b), "%llu", v);
    return wxString::FromUTF8(b);
}

// Split "host:port" (on the last ':'); defaults port 9883, host localhost.
static void ParseHostPort(const wxString &hp, wxString &host, int &port)
{
    host = hp;
    port = 9883;
    int idx = hp.Find(':', true);   // search from the right
    if (idx != wxNOT_FOUND)
    {
        host = hp.Mid(0, idx);
        long p = 0;
        if (hp.Mid(idx + 1).ToLong(&p) && p > 0 && p < 65536)
            port = (int)p;
    }
    host.Trim(true).Trim(false);
    if (host.IsEmpty())
        host = "localhost";
}

// Append s to out as a JSON string literal, escaping as needed.
static void json_str(wxString &out, const wxString &s)
{
    out << '"';
    for (size_t i = 0; i < s.length(); i++)
    {
        wxUniChar c = s[i];
        int v = (int)c.GetValue();
        switch (v)
        {
        case '"':  out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (v < 0x20)
                out << wxString::Format("\\u%04x", v);
            else
                out << c;
        }
    }
    out << '"';
}

// Pull a bare scalar token (number/true/false) that follows "key": in a small,
// trusted JSON object. Adequate for our fixed {ok, navmate_dt, ocpn_dt} reply.
static bool json_scalar(const std::string &s, const char *key, std::string &tok)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos)
        return false;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos)
        return false;
    p++;
    while (p < s.size() && isspace((unsigned char)s[p]))
        p++;
    size_t start = p;
    while (p < s.size() && s[p] != ',' && s[p] != '}' &&
           !isspace((unsigned char)s[p]))
        p++;
    tok = s.substr(start, p - start);
    return !tok.empty();
}

// Parse navMate's {ok, navmate_dt, ocpn_dt} reply. Returns false if malformed.
static bool ParseView(const wxString &body, bool &ok, long long &navmate_dt,
                      long long &ocpn_dt)
{
    std::string s = std::string(body.ToUTF8().data());
    std::string tok;
    if (!json_scalar(s, "navmate_dt", tok))
        return false;
    navmate_dt = atoll(tok.c_str());
    if (!json_scalar(s, "ocpn_dt", tok))
        return false;
    ocpn_dt = atoll(tok.c_str());
    ok = json_scalar(s, "ok", tok) && (tok == "true" || tok == "1");
    return true;
}

//----------------------------------------------------------------------------
//    The class factories, used to create and destroy instances of the PlugIn
//----------------------------------------------------------------------------

extern "C" DECL_EXP opencpn_plugin *create_pi(void *ppimgr)
{
    return new oESeriesPi(ppimgr);
}

extern "C" DECL_EXP void destroy_pi(opencpn_plugin *p)
{
    delete p;
}

//----------------------------------------------------------------------------
//    oESeries PlugIn Implementation
//----------------------------------------------------------------------------

oESeriesPi::oESeriesPi(void *ppimgr)
    : opencpn_plugin_120(ppimgr),
      m_config(nullptr),
      m_parent_window(nullptr),
      m_host_port(DEFAULT_HOST_PORT),
      m_debug_level(DEFAULT_DEBUG_LEVEL),
      m_timer(nullptr),
      m_http(nullptr),
      m_dt_ocpn(0),
      m_last_hash(0),
      m_have_hash(false),
      m_wp_count(0),
      m_want_post(false),
      m_navmate_dt(0),
      m_synced(false),
      m_reachable(false)
{
}

oESeriesPi::~oESeriesPi()
{
}

int oESeriesPi::Init()
{
    m_parent_window = GetOCPNCanvasWindow();
    m_config = reinterpret_cast<wxFileConfig *>(GetOCPNConfigObject());
    LoadConfig();

    oeLogInit();
    oeLog(0, 0, "Init: host=%s debug=%d",
          static_cast<const char *>(m_host_port.mb_str()), m_debug_level);

    wxLogMessage("oESeries: Init - navMate spoke plugin loaded (v%d.%d.%d); "
                 "host=%s debug=%d",
                 PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR,
                 PLUGIN_VERSION_PATCH,
                 static_cast<const char *>(m_host_port.mb_str()),
                 m_debug_level);

    // Start the off-main-thread HTTP client and the main-thread heartbeat.
    m_http = new HttpWorker();
    if (!m_http->Start())
    {
        oeLog(0, 0, "ERROR: HTTP worker failed to start");
        wxLogError("oESeries: HTTP worker failed to start");
        delete m_http;
        m_http = nullptr;
    }
    m_timer = new oeTimer(this);
    m_timer->Start(POLL_INTERVAL_MS);

    return (WANTS_PREFERENCES | WANTS_CONFIG);
}

bool oESeriesPi::DeInit()
{
    if (m_timer)
    {
        m_timer->Stop();
        delete m_timer;
        m_timer = nullptr;
    }
    if (m_http)
    {
        m_http->Shutdown();
        delete m_http;
        m_http = nullptr;
    }

    SaveConfig();
    oeLog(0, 0, "DeInit");
    wxLogMessage("oESeries: DeInit");
    oeLogClose();
    return true;
}

//----------------------------------------------------------------------------
//    Sync engine
//----------------------------------------------------------------------------

void oESeriesPi::EnumerateAndBuild()
{
    // Main-thread only: read OpenCPN's live waypoint model.
    wxArrayString guids = GetWaypointGUIDArray();
    std::vector<WpEntry> v;
    v.reserve(guids.GetCount());
    for (size_t i = 0; i < guids.GetCount(); i++)
    {
        PlugIn_Waypoint_Ex wp;
        if (GetSingleWaypointEx(guids[i], &wp))
        {
            WpEntry e;
            e.guid = wp.m_GUID;
            e.name = wp.m_MarkName;
            e.desc = wp.m_MarkDescription;
            e.icon = wp.IconName;
            e.lat = wp.m_lat;
            e.lon = wp.m_lon;
            e.visible = wp.IsVisible;
            v.push_back(e);
        }
    }

    std::sort(v.begin(), v.end(),
              [](const WpEntry &a, const WpEntry &b) { return a.guid < b.guid; });

    // One pass: FNV-1a hash over a canonical form, and the JSON array.
    unsigned long long h = 1469598103934665603ULL;   // FNV offset basis
    wxString arr;
    arr << "[";
    for (size_t i = 0; i < v.size(); i++)
    {
        const WpEntry &e = v[i];
        wxString slat = wxString::Format("%.6f", e.lat);
        wxString slon = wxString::Format("%.6f", e.lon);

        wxString canon;
        canon << e.guid << "|" << e.name << "|" << slat << "|" << slon << "|"
              << e.desc << "|" << e.icon << "|" << (e.visible ? "1" : "0")
              << "\n";
        wxScopedCharBuffer cb = canon.ToUTF8();
        for (size_t k = 0; k < cb.length(); k++)
        {
            h ^= (unsigned char)cb.data()[k];
            h *= 1099511628211ULL;   // FNV prime
        }

        if (i)
            arr << ",";
        arr << "{\"guid\":";
        json_str(arr, e.guid);
        arr << ",\"name\":";
        json_str(arr, e.name);
        arr << ",\"lat\":" << slat << ",\"lon\":" << slon;
        arr << ",\"description\":";
        json_str(arr, e.desc);
        arr << ",\"icon\":";
        json_str(arr, e.icon);
        arr << ",\"visible\":" << (e.visible ? "true" : "false");
        arr << "}";
    }
    arr << "]";

    bool changed = (!m_have_hash) || (h != m_last_hash);
    if (!changed)
        return;

    m_have_hash = true;
    m_last_hash = h;
    m_wp_count = (int)v.size();

    // Advance DT_ocpn: a strictly-increasing, date-time-ish token (epoch secs,
    // bumped past the last value so rapid edits still differ). Minted only here.
    long long now = (long long)time(NULL);
    if (now <= (long long)m_dt_ocpn)
        now = (long long)m_dt_ocpn + 1;
    m_dt_ocpn = (unsigned long long)now;

    m_payload = "{\"dt\":" + DtStr(m_dt_ocpn) + ",\"waypoints\":" + arr + "}";

    oeLog(1, 0, "inventory changed: %d waypoints, DT_ocpn=%s", m_wp_count,
          static_cast<const char *>(DtStr(m_dt_ocpn).mb_str()));
}

void oESeriesPi::OnTimer()
{
    // 1) Main-thread model read + change detection (advances DT on change).
    EnumerateAndBuild();

    if (!m_http)
        return;

    // 2) Consume any completed HTTP result from the worker.
    HttpResult res;
    if (m_http->TryGetResult(res))
    {
        if (res.ok)
        {
            if (!m_reachable)
            {
                m_reachable = true;
                wxLogMessage("oESeries: navMate reachable");
            }
            bool ok = false;
            long long ndt = 0, odt = 0;
            if (ParseView(res.body, ok, ndt, odt))
            {
                m_navmate_dt = ndt;
                bool matched = ((unsigned long long)odt == m_dt_ocpn);
                m_want_post = !matched;
                if (res.tag == TAG_POST)
                {
                    oeLog(matched ? 0 : 1, 1,
                          "POST ack: ocpn_dt=%lld mine=%s navmate_dt=%lld%s", odt,
                          static_cast<const char *>(DtStr(m_dt_ocpn).mb_str()),
                          ndt, matched ? "  [SYNCED]" : "");
                    if (matched && !m_synced)
                    {
                        m_synced = true;
                        wxLogMessage("oESeries: sync ok - navMate acknowledged "
                                     "DT=%lld (%d waypoints)", odt, m_wp_count);
                    }
                }
                else
                {
                    if (m_want_post)
                        m_synced = false;
                    oeLog(2, 1,
                          "GET view: navmate_dt=%lld ocpn_dt=%lld mine=%s "
                          "want_post=%d", ndt, odt,
                          static_cast<const char *>(DtStr(m_dt_ocpn).mb_str()),
                          (int)m_want_post);
                }
            }
            else
            {
                oeLog(1, 1, "parse failed (%s): %s",
                      res.tag == TAG_POST ? "POST" : "GET",
                      static_cast<const char *>(res.body.Left(200).mb_str()));
            }
        }
        else
        {
            if (m_reachable || m_navmate_dt == 0)
            {
                // Only announce the transition to unreachable once.
                if (m_reachable)
                    wxLogMessage("oESeries: navMate unreachable (%s), retrying",
                                 static_cast<const char *>(res.error.mb_str()));
                m_reachable = false;
            }
            oeLog(3, 1, "%s failed: %s", res.tag == TAG_POST ? "POST" : "GET",
                  static_cast<const char *>(res.error.mb_str()));
        }
    }

    // 3) If the worker is idle, issue the next request (single-flight).
    if (!m_http->IsBusy())
    {
        wxString host;
        int port;
        ParseHostPort(m_host_port, host, port);
        if (m_want_post && !m_payload.IsEmpty())
            m_http->Submit(TAG_POST, "POST", host, port, OCPN_API_PATH,
                           m_payload);
        else
            m_http->Submit(TAG_GET, "GET", host, port, OCPN_API_PATH,
                           wxEmptyString);
    }
}

bool oESeriesPi::LoadConfig()
{
    if (!m_config)
        return false;

    m_config->SetPath(CONFIG_PATH);
    m_host_port = m_config->Read("HostPort", DEFAULT_HOST_PORT);

    long debug = DEFAULT_DEBUG_LEVEL;
    m_config->Read("DebugLevel", &debug, DEFAULT_DEBUG_LEVEL);
    m_debug_level = static_cast<int>(debug);

    m_host_port.Trim(true).Trim(false);
    if (m_host_port.IsEmpty())
        m_host_port = DEFAULT_HOST_PORT;

    g_oeseries_debug_level = m_debug_level;
    return true;
}

bool oESeriesPi::SaveConfig()
{
    if (!m_config)
        return false;

    m_config->SetPath(CONFIG_PATH);
    m_config->Write("HostPort", m_host_port);
    m_config->Write("DebugLevel", m_debug_level);
    m_config->Flush();
    return true;
}

void oESeriesPi::ShowPreferencesDialog(wxWindow *parent)
{
    wxDialog dlg(parent, wxID_ANY, _("oESeries Preferences"), wxDefaultPosition,
                 wxDefaultSize, wxDEFAULT_DIALOG_STYLE);

    wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);

    wxFlexGridSizer *grid = new wxFlexGridSizer(2, 2, 8, 8);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(&dlg, wxID_ANY, _("navMate host:port")), 0,
              wxALIGN_CENTER_VERTICAL);
    wxTextCtrl *host_ctrl =
        new wxTextCtrl(&dlg, wxID_ANY, m_host_port, wxDefaultPosition,
                       wxSize(220, -1));
    grid->Add(host_ctrl, 1, wxEXPAND);

    grid->Add(new wxStaticText(&dlg, wxID_ANY, _("Debug level")), 0,
              wxALIGN_CENTER_VERTICAL);
    wxSpinCtrl *debug_ctrl =
        new wxSpinCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition,
                       wxDefaultSize, wxSP_ARROW_KEYS, 0, 9, m_debug_level);
    grid->Add(debug_ctrl, 0);

    top->Add(grid, 1, wxEXPAND | wxALL, 12);
    top->Add(dlg.CreateButtonSizer(wxOK | wxCANCEL), 0,
             wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    dlg.SetSizerAndFit(top);
    dlg.CenterOnParent();

    if (dlg.ShowModal() != wxID_OK)
        return;

    wxString new_host = host_ctrl->GetValue();
    new_host.Trim(true).Trim(false);
    if (new_host.IsEmpty())
        new_host = DEFAULT_HOST_PORT;
    int new_level = debug_ctrl->GetValue();

    bool changed = (new_host != m_host_port) || (new_level != m_debug_level);
    m_host_port = new_host;
    m_debug_level = new_level;
    g_oeseries_debug_level = m_debug_level;
    SaveConfig();

    if (changed)
        wxLogMessage("oESeries: prefs updated - host=%s debug=%d",
                     static_cast<const char *>(m_host_port.mb_str()),
                     m_debug_level);
}

int oESeriesPi::GetAPIVersionMajor()
{
    return atoi(API_VERSION);
}

int oESeriesPi::GetAPIVersionMinor()
{
    std::string v(API_VERSION);
    size_t dotpos = v.find('.');
    return atoi(v.substr(dotpos + 1).c_str());
}

int oESeriesPi::GetPlugInVersionMajor()
{
    return PLUGIN_VERSION_MAJOR;
}

int oESeriesPi::GetPlugInVersionMinor()
{
    return PLUGIN_VERSION_MINOR;
}

int oESeriesPi::GetPlugInVersionPatch()
{
    return PLUGIN_VERSION_PATCH;
}

int oESeriesPi::GetPlugInVersionPost()
{
    return PLUGIN_VERSION_TWEAK;
}

const char *oESeriesPi::GetPlugInVersionPre()
{
    return PKG_PRERELEASE;
}

const char *oESeriesPi::GetPlugInVersionBuild()
{
    return PKG_BUILD_INFO;
}

wxBitmap *oESeriesPi::GetPlugInBitmap()
{
    // The plugin-manager panel icon (Options -> Plugins). Since oESeries has no
    // toolbar and no chart overlay, this tile is its ONLY visual surface, so we
    // load a real icon rather than the empty default. Lazy, cached: OpenCPN may
    // call this repeatedly. The PNG ships in data/ (installed by the framework
    // to <shared>/opencpn/plugins/oESeries_pi/data/); GetPluginDataDir resolves
    // that root at runtime. A bare-DLL dev copy with no data/ just shows blank.
    if (!m_panel_bitmap.IsOk())
    {
        // OpenCPN inits the image handlers, but don't assume - a missing PNG
        // handler would make LoadFile fail silently.
        if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
            wxImage::AddHandler(new wxPNGHandler);

        wxString dir = GetPluginDataDir("oESeries_pi");
        if (!dir.IsEmpty())
        {
            wxString path = dir + wxFileName::GetPathSeparator()
                          + "data" + wxFileName::GetPathSeparator()
                          + "oeSeries_panel_icon.png";
            wxImage img;
            if (wxFileExists(path) && img.LoadFile(path, wxBITMAP_TYPE_PNG))
            {
                m_panel_bitmap = wxBitmap(img);
                oeLog(0, 0, "panel icon loaded: %s",
                      (const char *)path.mb_str());
            }
            else
            {
                oeLog(0, 0, "panel icon NOT found: %s",
                      (const char *)path.mb_str());
            }
        }
    }
    return &m_panel_bitmap;
}

wxString oESeriesPi::GetCommonName()
{
    return PLUGIN_API_NAME;
}

wxString oESeriesPi::GetShortDescription()
{
    return PKG_SUMMARY;
}

wxString oESeriesPi::GetLongDescription()
{
    return PKG_DESCRIPTION;
}
