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
#include <memory>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include "config.h"
#include "oeSeries_pi.h"
#include "oeSeries_log.h"
#include "oeSeries_http.h"
#include "ocpn_plugin.h"

// Vendored single-header JSON (nlohmann/json v3.11.3, src/json.hpp). Replaces the
// former hand-rolled serializer: correct UTF-8 escaping and full-precision numbers
// straight from the model doubles (protocol.md sec 2A).
#include "json.hpp"

// The api-20 header WX_DECLARE_LISTs these waypoint lists, but only OpenCPN core
// carries the matching WX_DEFINE_LIST - the node vtable (DeleteData) isn't exported
// to plugins. Enumeration only ITERATES core-owned lists (inline, fine), but to
// CONSTRUCT one plugin-side for a route/track push we must emit our own local
// definition of the node methods here.
#include <wx/listimpl.cpp>
WX_DEFINE_LIST(Plugin_WaypointExV2List);
WX_DEFINE_LIST(Plugin_WaypointList);

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

// A snapshot of one waypoint's sync-relevant fields (the two-way "carried" set,
// protocol.md sec 6). `visible` is dropped in v1; `created_ts` (UTC epoch secs,
// 0 if unset) is added.
struct WpEntry
{
    wxString guid;
    wxString name;
    wxString desc;
    wxString icon;
    double lat;
    double lon;
    long long created_ts;
};

// wxString -> UTF-8 std::string, for nlohmann string assignment.
static std::string ToU8(const wxString &s)
{
    return std::string(s.ToUTF8().data());
}

// Fold a wxString's UTF-8 bytes into a running FNV-1a hash.
static void FnvUpdate(unsigned long long &h, const wxString &s)
{
    wxScopedCharBuffer cb = s.ToUTF8();
    for (size_t k = 0; k < cb.length(); k++)
    {
        h ^= (unsigned char)cb.data()[k];
        h *= 1099511628211ULL;   // FNV prime
    }
}

// Copy the carried mark fields (sec 6) out of any api-20 waypoint struct sharing
// the PlugIn_Waypoint_Ex[V2] field names (marks and route vertices both qualify).
template <typename WP>
static WpEntry WpEntryFrom(const WP &wp)
{
    WpEntry e;
    e.guid = wp.m_GUID;
    e.name = wp.m_MarkName;
    e.desc = wp.m_MarkDescription;
    e.icon = wp.IconName;
    e.lat = wp.m_lat;
    e.lon = wp.m_lon;
    // created_ts = m_CreateTime (UTC); 0 when unset (sec 2A: 0 = unknown).
    e.created_ts = wp.m_CreateTime.IsValid()
                 ? (long long)wp.m_CreateTime.GetTicks() : 0;
    return e;
}

// Build the sec-2A `mark` JSON for e, folding its canonical form into h. The hash
// uses %.6f lat/lon (a lossy change DETECTOR); the wire carries the full-precision
// double verbatim (sec 2A: "NO wire rounding, or coordinates drift every
// round-trip"). Reused for standalone marks[] and for embedded route vertices.
static nlohmann::json MarkToJson(const WpEntry &e, unsigned long long &h)
{
    wxString canon;
    canon << "M|" << e.guid << "|" << e.name << "|"
          << wxString::Format("%.6f", e.lat) << "|"
          << wxString::Format("%.6f", e.lon) << "|"
          << e.desc << "|" << e.icon << "|"
          << wxString::Format("%lld", e.created_ts) << "\n";
    FnvUpdate(h, canon);

    nlohmann::json m;
    m["guid"] = ToU8(e.guid);
    m["name"] = ToU8(e.name);
    m["lat"] = e.lat;   // full-precision double, not the %.6f hash form
    m["lon"] = e.lon;
    m["description"] = ToU8(e.desc);
    m["icon"] = ToU8(e.icon);
    m["created_ts"] = e.created_ts;
    return m;
}

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
      m_route_count(0),
      m_track_count(0),
      m_vertices_seen(0),
      m_have_inventory(false),
      m_want_post(false),
      m_navmate_dt(0),
      m_synced(false),
      m_reachable(false),
      m_last_applied_batch(0),
      m_echo_baseline_dt(0),
      m_echo_baseline_hash(0)
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
    // Main-thread only: read OpenCPN's live model. ONE FNV-1a hash spans marks +
    // routes + tracks, so a change to any carried field or structure re-syncs
    // (sec 12). Type prefixes (M|R|P|T|TP|) keep the per-type canon forms distinct.
    unsigned long long h = 1469598103934665603ULL;   // FNV offset basis

    // ---- marks: FREE-STANDING waypoints only (sec 5) ----
    // GetWaypointGUIDArray returns every RoutePoint (marks AND route vertices);
    // GetFSStatus() splits them. Pure vertices are delivered only INSIDE their
    // route (embedded below), never in marks[] - this is the ~84-double-count fix.
    wxArrayString wguids = GetWaypointGUIDArray();
    std::vector<WpEntry> marks_v;
    marks_v.reserve(wguids.GetCount());
    for (size_t i = 0; i < wguids.GetCount(); i++)
    {
        PlugIn_Waypoint_Ex wp;
        if (GetSingleWaypointEx(wguids[i], &wp) && wp.GetFSStatus())
            marks_v.push_back(WpEntryFrom(wp));
    }
    std::sort(marks_v.begin(), marks_v.end(),
              [](const WpEntry &a, const WpEntry &b) { return a.guid < b.guid; });
    int fs_pre_dedup = (int)marks_v.size();
    // GetWaypointGUIDArray() lists each shared route-member point MORE THAN ONCE
    // (once standalone + once per route membership), so the free-standing set holds
    // duplicate GUIDs (bench-observed: 183 raw -> 84 dups of the 84 route members).
    // marks[] must be a DISTINCT set (sec 2A / sec 4: one object per GUID), so
    // collapse adjacent duplicates after the guid sort. Route members still ride as
    // bare refs and resolve to the single surviving marks[] entry.
    marks_v.erase(std::unique(marks_v.begin(), marks_v.end(),
                  [](const WpEntry &a, const WpEntry &b) { return a.guid == b.guid; }),
                  marks_v.end());
    nlohmann::json marks = nlohmann::json::array();
    for (size_t i = 0; i < marks_v.size(); i++)
        marks.push_back(MarkToJson(marks_v[i], h));

    // ---- routes: ordered members, refs + embedded vertices (sec 2A outbound) ----
    wxArrayString rguids = GetRouteGUIDArray();
    rguids.Sort();   // stable hash order (guids sorted; point order is preserved)
    nlohmann::json routes = nlohmann::json::array();
    int route_pts_total = 0;   // diag: total route members seen
    int route_verts = 0;       // diag: members classified pure-vertex (embedded)
    for (size_t i = 0; i < rguids.GetCount(); i++)
    {
        std::unique_ptr<PlugIn_Route_ExV2> route = GetRouteExV2_Plugin(rguids[i]);
        if (!route)
            continue;

        wxString rcanon;
        rcanon << "R|" << route->m_GUID << "|" << route->m_NameString << "|"
               << route->m_Description << "\n";
        FnvUpdate(h, rcanon);

        nlohmann::json rj;
        rj["guid"] = ToU8(route->m_GUID);
        rj["name"] = ToU8(route->m_NameString);
        rj["description"] = ToU8(route->m_Description);

        nlohmann::json pts = nlohmann::json::array();
        int position = 0;
        if (route->pWaypointList)
        {
            Plugin_WaypointExV2List::compatibility_iterator node =
                route->pWaypointList->GetFirst();
            while (node)
            {
                PlugIn_Waypoint_ExV2 *wp = node->GetData();
                if (wp)
                {
                    nlohmann::json p;
                    p["guid"] = ToU8(wp->m_GUID);
                    p["position"] = position;

                    wxString pcanon;
                    pcanon << "P|" << wp->m_GUID << "|" << position << "\n";
                    FnvUpdate(h, pcanon);

                    // A pure vertex (not free-standing, in no marks[]) embeds its
                    // full mark; a free-standing member rides as a bare
                    // {guid, position} ref (its full object is in marks[]).
                    route_pts_total++;
                    if (!wp->GetFSStatus())
                    {
                        p["mark"] = MarkToJson(WpEntryFrom(*wp), h);
                        route_verts++;
                    }

                    pts.push_back(std::move(p));
                    position++;
                }
                node = node->GetNext();
            }
        }
        rj["points"] = std::move(pts);
        oeLog(3, 2, "route '%s': %d points",
              static_cast<const char *>(route->m_NameString.mb_str()), position);
        routes.push_back(std::move(rj));
    }

    // ---- tracks: flat ordered points {lat, lon, ts} (sec 11) ----
    wxArrayString tguids = GetTrackGUIDArray();
    tguids.Sort();
    nlohmann::json tracks = nlohmann::json::array();
    for (size_t i = 0; i < tguids.GetCount(); i++)
    {
        std::unique_ptr<PlugIn_Track> trk = GetTrack_Plugin(tguids[i]);
        if (!trk)
            continue;

        wxString tcanon;
        tcanon << "T|" << trk->m_GUID << "|" << trk->m_NameString << "\n";
        FnvUpdate(h, tcanon);

        nlohmann::json tj;
        tj["guid"] = ToU8(trk->m_GUID);
        tj["name"] = ToU8(trk->m_NameString);

        nlohmann::json pts = nlohmann::json::array();
        if (trk->pWaypointList)
        {
            Plugin_WaypointList::compatibility_iterator node =
                trk->pWaypointList->GetFirst();
            while (node)
            {
                PlugIn_Waypoint *wp = node->GetData();
                if (wp)
                {
                    long long ts = wp->m_CreateTime.IsValid()
                                 ? (long long)wp->m_CreateTime.GetTicks() : 0;
                    nlohmann::json p;
                    p["lat"] = wp->m_lat;   // full precision
                    p["lon"] = wp->m_lon;
                    p["ts"] = ts;

                    wxString pcanon;
                    pcanon << "TP|" << wxString::Format("%.6f", wp->m_lat) << "|"
                           << wxString::Format("%.6f", wp->m_lon) << "|"
                           << wxString::Format("%lld", ts) << "\n";
                    FnvUpdate(h, pcanon);

                    pts.push_back(std::move(p));
                }
                node = node->GetNext();
            }
        }
        int tcount = (int)pts.size();
        tj["points"] = std::move(pts);
        oeLog(3, 2, "track '%s': %d points",
              static_cast<const char *>(trk->m_NameString.mb_str()), tcount);
        tracks.push_back(std::move(tj));
    }

    bool changed = (!m_have_hash) || (h != m_last_hash);
    if (!changed)
        return;

    m_have_hash = true;
    m_last_hash = h;
    m_wp_count = (int)marks_v.size();
    m_route_count = (int)routes.size();
    m_track_count = (int)tracks.size();
    m_vertices_seen = route_verts;

    // Advance DT_ocpn: a strictly-increasing epoch-seconds token, bumped past the
    // last value so rapid edits still differ. Minted only here.
    long long now = (long long)time(NULL);
    if (now <= (long long)m_dt_ocpn)
        now = (long long)m_dt_ocpn + 1;
    m_dt_ocpn = (unsigned long long)now;

    // Cache the three inventory arrays (pure-ASCII, ensure_ascii=true). The POST
    // body {dt, marks, routes, tracks, results} is assembled from these plus any
    // pending results[] in BuildPostBody() at send time (sec 2A).
    m_marks_json = wxString::FromUTF8(marks.dump(-1, ' ', true).c_str());
    m_routes_json = wxString::FromUTF8(routes.dump(-1, ' ', true).c_str());
    m_tracks_json = wxString::FromUTF8(tracks.dump(-1, ' ', true).c_str());
    m_have_inventory = true;

    oeLog(1, 0, "inventory changed: %d marks, %d routes, %d tracks, DT_ocpn=%s",
          m_wp_count, m_route_count, m_track_count,
          static_cast<const char *>(DtStr(m_dt_ocpn).mb_str()));

    // FS-split diagnostics (sec 5): raw enumerated GUIDs vs the free-standing
    // subset kept in marks[], and how route members classified (bare ref vs
    // embedded pure vertex). Distinguishes "no pure vertices exist" from
    // "GetFSStatus() failed to discriminate" - the R1-class question.
    oeLog(2, 1, "marks split: raw_guids=%d fs=%d distinct=%d dups_removed=%d "
                "vertices_skipped=%d",
          (int)wguids.GetCount(), fs_pre_dedup, (int)marks_v.size(),
          fs_pre_dedup - (int)marks_v.size(),
          (int)wguids.GetCount() - fs_pre_dedup);
    oeLog(2, 1, "route members: %d total, %d embedded_vertices, %d bare_refs",
          route_pts_total, route_verts, route_pts_total - route_verts);
}

// Overlay only the PRESENT mark fields from a command's `fields` onto wp
// (merge-on-apply, sec 8: unspecified fields keep their live values).
static void ApplyMarkFields(PlugIn_Waypoint_ExV2 &wp, const nlohmann::json &f)
{
    if (!f.is_object())
        return;
    if (f.contains("name") && f["name"].is_string())
        wp.m_MarkName = wxString::FromUTF8(f["name"].get<std::string>().c_str());
    if (f.contains("description") && f["description"].is_string())
        wp.m_MarkDescription =
            wxString::FromUTF8(f["description"].get<std::string>().c_str());
    if (f.contains("icon") && f["icon"].is_string())
        wp.IconName = wxString::FromUTF8(f["icon"].get<std::string>().c_str());
    if (f.contains("lat") && f["lat"].is_number())
        wp.m_lat = f["lat"].get<double>();
    if (f.contains("lon") && f["lon"].is_number())
        wp.m_lon = f["lon"].get<double>();
    if (f.contains("created_ts") && f["created_ts"].is_number_integer())
    {
        long long ts = f["created_ts"].get<long long>();
        if (ts > 0)
            wp.m_CreateTime = wxDateTime((time_t)ts);
    }
}

// Assemble the full POST body from the cached inventory arrays + pending results.
wxString oESeriesPi::BuildPostBody()
{
    wxString b;
    b << "{\"dt\":" << DtStr(m_dt_ocpn)
      << ",\"marks\":"   << (m_marks_json.IsEmpty()  ? wxString("[]") : m_marks_json)
      << ",\"routes\":"  << (m_routes_json.IsEmpty() ? wxString("[]") : m_routes_json)
      << ",\"tracks\":"  << (m_tracks_json.IsEmpty() ? wxString("[]") : m_tracks_json)
      << ",\"results\":"
      << (m_pending_results.IsEmpty() ? wxString("[]") : m_pending_results)
      << "}";
    return b;
}

// True if a route with this GUID already exists (idempotency: update vs add; a
// re-Add would duplicate every point, sec 8).
static bool RouteExists(const wxString &guid)
{
    wxArrayString a = GetRouteGUIDArray();
    for (size_t i = 0; i < a.GetCount(); i++)
        if (a[i] == guid)
            return true;
    return false;
}

// True if a track with this GUID already exists. Tracks are immutable, so an
// existing guid means "already applied" - re-adding it (which happens when a
// command batch is re-GET before its results retire it) duplicates the track and
// corrupts the model. This guard makes track add idempotent (cf. RouteExists).
static bool TrackExists(const wxString &guid)
{
    wxArrayString a = GetTrackGUIDArray();
    for (size_t i = 0; i < a.GetCount(); i++)
        if (a[i] == guid)
            return true;
    return false;
}

// Build a PlugIn_Route_ExV2 from a full-embed inbound route object (sec 2A: every
// point carries its full `mark`) and add or update it. Each vertex's m_GUID is set
// verbatim from the embedded mark - whether AddPlugInRouteExV2 PRESERVES it is the
// R2 bench question, read back via diag object. Copy-semantics assumed (OpenCPN
// copies the struct into its own model), so we free our temporaries afterward and
// null pWaypointList so the stack route's dtor can't double-free.
static bool ApplyRouteObject(const wxString &guid, const nlohmann::json &fields,
                             bool exists, wxString &err)
{
    if (!fields.is_object())
    {
        err = "missing route fields";
        return false;
    }
    PlugIn_Route_ExV2 route;
    route.m_GUID = guid;
    if (fields.contains("name") && fields["name"].is_string())
        route.m_NameString =
            wxString::FromUTF8(fields["name"].get<std::string>().c_str());
    if (fields.contains("description") && fields["description"].is_string())
        route.m_Description =
            wxString::FromUTF8(fields["description"].get<std::string>().c_str());

    Plugin_WaypointExV2List *lst = new Plugin_WaypointExV2List;
    if (fields.contains("points") && fields["points"].is_array())
    {
        for (const nlohmann::json &pt : fields["points"])
        {
            PlugIn_Waypoint_ExV2 *wp = new PlugIn_Waypoint_ExV2();
            const nlohmann::json &m =
                (pt.contains("mark") && pt["mark"].is_object()) ? pt["mark"] : pt;
            std::string pg = m.value("guid", pt.value("guid", std::string()));
            wp->m_GUID = wxString::FromUTF8(pg.c_str());   // R2: preserve vertex guid
            ApplyMarkFields(*wp, m);
            lst->Append(wp);
        }
    }
    route.pWaypointList = lst;
    bool ok = exists ? UpdatePlugInRouteExV2(&route) : AddPlugInRouteExV2(&route);
    // Free our temporary waypoints (OpenCPN copied them into its own model), then
    // the list. We delete the data ourselves rather than wxList DeleteContents()
    // because the node's DeleteData() isn't exported to plugins (link error).
    for (Plugin_WaypointExV2List::compatibility_iterator n = lst->GetFirst(); n;
         n = n->GetNext())
        delete n->GetData();
    delete lst;
    route.pWaypointList = nullptr;
    if (!ok)
        err = exists ? "UpdatePlugInRouteExV2 failed" : "AddPlugInRouteExV2 failed";
    return ok;
}

// Build a PlugIn_Track from a flat track object {name, points:[{lat,lon,ts}]} and
// add it. Same copy-semantics/cleanup discipline as routes.
static bool ApplyTrackObject(const wxString &guid, const nlohmann::json &fields,
                             wxString &err)
{
    if (!fields.is_object())
    {
        err = "missing track fields";
        return false;
    }
    PlugIn_Track track;
    track.m_GUID = guid;
    if (fields.contains("name") && fields["name"].is_string())
        track.m_NameString =
            wxString::FromUTF8(fields["name"].get<std::string>().c_str());

    Plugin_WaypointList *lst = new Plugin_WaypointList;
    if (fields.contains("points") && fields["points"].is_array())
    {
        for (const nlohmann::json &pt : fields["points"])
        {
            PlugIn_Waypoint *wp = new PlugIn_Waypoint();
            if (pt.contains("lat") && pt["lat"].is_number())
                wp->m_lat = pt["lat"].get<double>();
            if (pt.contains("lon") && pt["lon"].is_number())
                wp->m_lon = pt["lon"].get<double>();
            if (pt.contains("ts") && pt["ts"].is_number_integer())
            {
                long long ts = pt["ts"].get<long long>();
                if (ts > 0)
                    wp->m_CreateTime = wxDateTime((time_t)ts);
            }
            lst->Append(wp);
        }
    }
    track.pWaypointList = lst;
    oeLog(2, 2, "track: pre-AddPlugInTrack, %d points", (int)lst->GetCount());
    bool ok = AddPlugInTrack(&track, true);
    oeLog(2, 2, "track: post-AddPlugInTrack ok=%d", (int)ok);
    for (Plugin_WaypointList::compatibility_iterator n = lst->GetFirst(); n;
         n = n->GetNext())
        delete n->GetData();
    delete lst;
    track.pWaypointList = nullptr;
    oeLog(2, 2, "track: cleanup done");
    if (!ok)
        err = "AddPlugInTrack failed";
    return ok;
}

// M3: parse a GET view's commands[] and apply each on the MAIN thread. Mutating
// mark ops use merge-on-apply (sec 8); diag ops are read-only -> results[].data.
// Idempotent (add=upsert, update-missing=err, delete-missing=ok), so re-applying
// an un-retired command batch is safe. Results are stashed for the next POST.
void oESeriesPi::ApplyGetView(const wxString &body)
{
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(std::string(body.ToUTF8().data()));
    }
    catch (...)
    {
        return;
    }
    if (!j.contains("commands") || !j["commands"].is_array() ||
        j["commands"].empty())
        return;

    // Mark this batch BEFORE processing its commands, so a diag(state) command IN
    // this same batch reports the current batch id and the PRE-echo {dt, hash}
    // baseline (the marker the hub compares the coming echo against) - not the
    // stale pre-batch zeros. echo_baseline is the pre-echo snapshot: applying the
    // batch's mutations advances DT on the NEXT enumerate, and that reappearance is
    // the round-trip (sec 2A echo invariant).
    m_last_applied_batch += 1;
    m_echo_baseline_dt = (long long)m_dt_ocpn;
    m_echo_baseline_hash = m_last_hash;

    nlohmann::json results = nlohmann::json::array();
    int applied = 0;
    for (const nlohmann::json &cmd : j["commands"])
    {
        std::string op = cmd.value("op", std::string());
        std::string type = cmd.value("type", std::string());
        std::string guid = cmd.value("guid", std::string());

        // Logged (and flushed) BEFORE the apply so a crash leaves a breadcrumb of
        // exactly which command was in flight.
        oeLog(2, 1, "apply cmd: op=%s type=%s guid=%s", op.c_str(), type.c_str(),
              guid.c_str());

        nlohmann::json r;
        r["guid"] = guid;
        r["op"] = op;
        r["ok"] = false;
        r["error"] = std::string();

        if (op == "diag")
        {
            // Read-only observability channel -> results[].data.
            r["ok"] = true;
            nlohmann::json d;
            if (type == "inventory")
            {
                d["dt_ocpn"] = (long long)m_dt_ocpn;
                d["hash"] = std::string(DtStr(m_last_hash).mb_str());
                d["marks"] = m_wp_count;
                d["vertices"] = m_vertices_seen;
                d["routes"] = m_route_count;
                d["tracks"] = m_track_count;
                d["layer_seen"] = false;   // no layer detection built yet (R1)
            }
            else if (type == "state")
            {
                d["reachable"] = m_reachable;
                d["synced"] = m_synced;
                d["want_post"] = m_want_post;
                d["last_applied_batch"] = m_last_applied_batch;
                nlohmann::json eb;
                eb["dt"] = m_echo_baseline_dt;
                eb["hash"] = std::string(DtStr(m_echo_baseline_hash).mb_str());
                d["echo_baseline"] = eb;
            }
            else if (type == "object")
            {
                wxString g = wxString::FromUTF8(guid.c_str());
                PlugIn_Waypoint_ExV2 wp;
                std::unique_ptr<PlugIn_Route_ExV2> route;
                std::unique_ptr<PlugIn_Track> trk;
                unsigned long long dummy = 0;
                if (GetSingleWaypointExV2(g, &wp))
                {
                    d = MarkToJson(WpEntryFrom(wp), dummy);
                }
                else if ((route = GetRouteExV2_Plugin(g)))
                {
                    d["guid"] = ToU8(route->m_GUID);
                    d["name"] = ToU8(route->m_NameString);
                    d["description"] = ToU8(route->m_Description);
                    nlohmann::json pts = nlohmann::json::array();
                    int position = 0;
                    if (route->pWaypointList)
                    {
                        Plugin_WaypointExV2List::compatibility_iterator node =
                            route->pWaypointList->GetFirst();
                        while (node)
                        {
                            PlugIn_Waypoint_ExV2 *rp = node->GetData();
                            if (rp)
                            {
                                nlohmann::json p;
                                p["guid"] = ToU8(rp->m_GUID);
                                p["position"] = position;
                                p["mark"] = MarkToJson(WpEntryFrom(*rp), dummy);
                                pts.push_back(std::move(p));
                                position++;
                            }
                            node = node->GetNext();
                        }
                    }
                    d["points"] = std::move(pts);
                }
                else if ((trk = GetTrack_Plugin(g)))
                {
                    d["guid"] = ToU8(trk->m_GUID);
                    d["name"] = ToU8(trk->m_NameString);
                    int n = 0;
                    long long first_ts = 0, last_ts = 0;
                    if (trk->pWaypointList)
                    {
                        Plugin_WaypointList::compatibility_iterator node =
                            trk->pWaypointList->GetFirst();
                        while (node)
                        {
                            PlugIn_Waypoint *tp = node->GetData();
                            if (tp)
                            {
                                long long ts = tp->m_CreateTime.IsValid()
                                    ? (long long)tp->m_CreateTime.GetTicks() : 0;
                                if (n == 0)
                                    first_ts = ts;
                                last_ts = ts;
                                n++;
                            }
                            node = node->GetNext();
                        }
                    }
                    d["n_points"] = n;
                    d["first_ts"] = first_ts;
                    d["last_ts"] = last_ts;
                }
                else
                {
                    d["found"] = false;
                }
            }
            r["data"] = std::move(d);
            results.push_back(std::move(r));
            continue;
        }

        // Mutating ops. Increment (i): marks only; routes/tracks in (ii).
        if (type == "mark")
        {
            wxString g = wxString::FromUTF8(guid.c_str());
            if (op == "delete")
            {
                DeleteSingleWaypoint(g);   // absent -> ensure-absent, still ok
                r["ok"] = true;
            }
            else if (op == "add" || op == "update")
            {
                PlugIn_Waypoint_ExV2 wp;
                bool exists = GetSingleWaypointExV2(g, &wp);
                if (!exists && op == "update")
                {
                    r["error"] = "update of missing GUID";
                }
                else if (!exists)   // add of a new mark
                {
                    PlugIn_Waypoint_ExV2 nw;
                    nw.m_GUID = g;
                    if (cmd.contains("fields"))
                        ApplyMarkFields(nw, cmd["fields"]);
                    r["ok"] = AddSingleWaypointExV2(&nw, true);
                    if (!r["ok"].get<bool>())
                        r["error"] = "AddSingleWaypointExV2 failed";
                }
                else   // exists -> merge-on-apply (add=upsert or update)
                {
                    if (cmd.contains("fields"))
                        ApplyMarkFields(wp, cmd["fields"]);
                    r["ok"] = UpdateSingleWaypointExV2(&wp);
                    if (!r["ok"].get<bool>())
                        r["error"] = "UpdateSingleWaypointExV2 failed";
                }
            }
            else
            {
                r["error"] = "unknown op";
            }
            if (r["ok"].get<bool>())
                applied++;
        }
        else if (type == "route")
        {
            wxString g = wxString::FromUTF8(guid.c_str());
            if (op == "delete")
            {
                DeletePlugInRoute(g);   // absent -> ensure-absent
                r["ok"] = true;
            }
            else if (op == "add" || op == "update")
            {
                bool exists = RouteExists(g);
                if (!exists && op == "update")
                {
                    r["error"] = "update of missing route GUID";
                }
                else
                {
                    wxString err;
                    bool ok = ApplyRouteObject(
                        g, cmd.value("fields", nlohmann::json::object()), exists,
                        err);
                    r["ok"] = ok;
                    if (!ok)
                        r["error"] = std::string(err.mb_str());
                }
            }
            else
            {
                r["error"] = "unknown op";
            }
            if (r["ok"].get<bool>())
                applied++;
        }
        else if (type == "track")
        {
            wxString g = wxString::FromUTF8(guid.c_str());
            if (op == "delete")
            {
                DeletePlugInTrack(g);
                r["ok"] = true;
            }
            else if (op == "add")
            {
                if (TrackExists(g))
                {
                    // Idempotent: already present (e.g. a re-GET before retire).
                    // Re-Adding would duplicate + corrupt the model.
                    r["ok"] = true;
                }
                else
                {
                    wxString err;
                    bool ok = ApplyTrackObject(
                        g, cmd.value("fields", nlohmann::json::object()), err);
                    r["ok"] = ok;
                    if (!ok)
                        r["error"] = std::string(err.mb_str());
                }
            }
            else
            {
                r["error"] = "track update unsupported (tracks immutable, sec 11)";
            }
            if (r["ok"].get<bool>())
                applied++;
        }
        else
        {
            r["error"] = "unknown type";
        }
        results.push_back(std::move(r));
    }

    m_pending_results =
        wxString::FromUTF8(results.dump(-1, ' ', true).c_str());
    m_want_post = true;   // force a POST so results[] (+ any echo) go back

    oeLog(1, 0, "applied command batch #%lld: %d mutating-ok of %d commands",
          m_last_applied_batch, applied, (int)j["commands"].size());
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
                    // results[] (if any) rode this POST; navMate has them now.
                    if (!m_pending_results.IsEmpty())
                    {
                        oeLog(1, 1, "results[] delivered to hub, clearing pending");
                        m_pending_results.Clear();
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
                    // M3: apply any commands[] the hub queued (main thread). Sets
                    // m_pending_results + forces m_want_post if it applied a batch.
                    ApplyGetView(res.body);
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
        if (m_want_post && m_have_inventory)
            m_http->Submit(TAG_POST, "POST", host, port, OCPN_API_PATH,
                           BuildPostBody());
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
