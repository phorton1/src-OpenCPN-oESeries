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
#include "oe_buildnum.h"   // generated per build: PLUGIN_VERSION_FULL "X.Y.Z.NNN"
#include "oeSeries_pi.h"
#include "oeSeries_log.h"
#include "oeSeries_http.h"
#include "ocpn_plugin.h"

// Vendored single-header JSON (nlohmann/json v3.11.3, src/json.hpp). Replaces the
// former hand-rolled serializer: correct UTF-8 escaping and full-precision numbers
// straight from the model doubles (protocol.md sec 2A).
#include "json.hpp"

// S3 symbol channel: self-verified SHA-256 (icon_hash / byte_hash, sec 7) + wx
// base64 and memory streams for PNG <-> base64 icon transcode.
#include "oe_sha256.h"
#include <wx/base64.h>
#include <wx/mstream.h>

// The api-20 header WX_DECLARE_LISTs these waypoint lists, but only OpenCPN core
// carries the matching WX_DEFINE_LIST - the node vtable (DeleteData) isn't exported
// to plugins. Enumeration only ITERATES core-owned lists (inline, fine), but to
// CONSTRUCT one plugin-side for a route/track push we must emit our own local
// definition of the node methods here.
#include <wx/listimpl.cpp>
WX_DEFINE_LIST(Plugin_WaypointExV2List);
WX_DEFINE_LIST(Plugin_WaypointList);
WX_DEFINE_LIST(Plugin_HyperlinkList);   // for rebuilding m_HyperlinkList on apply (S2)

// wxColour <-> "#RRGGBB" wire form (sec 2A: the ONE color on the wire, range rings).
static wxString ColorToHex(const wxColour &c)
{
    if (!c.IsOk())
        return wxString("#000000");
    return wxString::Format("#%02X%02X%02X", c.Red(), c.Green(), c.Blue());
}
static wxColour HexToColor(const wxString &s)
{
    wxColour c(s);   // wxColour parses "#RRGGBB"
    return c.IsOk() ? c : wxColour(0, 0, 0);
}

// Preference keys / defaults
static const char *const CONFIG_PATH = "/PlugIns/oESeries";
// Dev default: navMate runs its dev navServer on 9883 (packaged is 9873).
static const char *const DEFAULT_HOST_PORT = "localhost:9883";
static const long DEFAULT_DEBUG_LEVEL = 0;

// Heartbeat cadence and the endpoint. One request per tick, single-flight.
static const int POLL_INTERVAL_MS = 2000;
static const char *const OCPN_API_PATH = "/api/ocpn";
static const char *const OCPN_ICONS_PATH = "/api/ocpn?icons=1";
static const int TAG_GET = 1;
static const int TAG_POST = 2;
static const int TAG_ICONS = 3;   // GET ?icons=1 (Direction B pull, sec 7)

// navMate keys its injected icons with this prefix (sec 7): a table-free magic
// recognizer, coexisting with OpenCPN built-ins. The plugin filters nm:* out of
// icon_hash (Direction A self-echo) and gates nm:* command apply on registration.
static const char *const NM_ICON_PREFIX = "nm:";

// The wire protocol version this plugin speaks (protocol.md "Versioning"). Self-announced
// in every POST; the hub announces its own in the GET view. MAJOR.MINOR; absent == "1.0"
// baseline. Soft floor: below our MAJOR we warn but keep functioning.
static const char *const OE_PROTOCOL_VERSION = "1.0";

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

// One hyperlink (sec 2A mark.hyperlinks[] <-> Plugin_Hyperlink).
struct HlEntry
{
    wxString desc;   // DescrText
    wxString link;   // Link
    wxString type;   // Type
};

// A snapshot of one waypoint's sync-relevant fields (protocol.md sec 6). Carries
// the A fields (guid/name/lat/lon/description/icon/created_ts) plus the full
// OpenCPN-only B superset (sec 2A extended mark), all read from PlugIn_Waypoint_ExV2.
struct WpEntry
{
    // A (mappable, carried both ways)
    wxString guid;
    wxString name;
    wxString desc;
    wxString icon;
    double lat;
    double lon;
    long long created_ts;
    // B (OpenCPN-only, spoke-carried; sec 6)
    bool visible;           // IsVisible
    bool name_shown;        // IsNameVisible
    bool active;            // IsActive [R, up-only]
    double scamin;          // scamin
    bool scamin_on;         // b_useScamin
    double scamax;          // scamax
    double arrival_radius;  // m_WaypointArrivalRadius
    double planned_speed;   // m_PlannedSpeed
    long long etd;          // m_ETD epoch secs; 0 = unset
    wxString tide_station;  // m_TideStation
    int rr_count;           // nrange_rings
    double rr_space;        // RangeRingSpace
    int rr_units;           // RangeRingSpaceUnits (0:nm 1:km)
    wxString rr_color;      // RangeRingColor "#RRGGBB"
    bool rr_show;           // m_bShowWaypointRangeRings
    std::vector<HlEntry> hyperlinks;
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

// Copy the carried mark fields (sec 6, A + full B superset) out of a live
// PlugIn_Waypoint_ExV2 (marks and route vertices both qualify - ExV2 exposes every
// reachable field; that is why enumeration reads ExV2, not the V1 Ex struct).
static WpEntry WpEntryFrom(const PlugIn_Waypoint_ExV2 &wp)
{
    WpEntry e;
    // A
    e.guid = wp.m_GUID;
    e.name = wp.m_MarkName;
    e.desc = wp.m_MarkDescription;
    e.icon = wp.IconName;
    e.lat = wp.m_lat;
    e.lon = wp.m_lon;
    // created_ts = m_CreateTime (UTC); 0 when unset (sec 2A: 0 = unknown).
    e.created_ts = wp.m_CreateTime.IsValid()
                 ? (long long)wp.m_CreateTime.GetTicks() : 0;
    // B
    e.visible = wp.IsVisible;
    e.name_shown = wp.IsNameVisible;
    e.active = wp.IsActive;
    e.scamin = wp.scamin;
    e.scamin_on = wp.b_useScamin;
    e.scamax = wp.scamax;
    e.arrival_radius = wp.m_WaypointArrivalRadius;
    e.planned_speed = wp.m_PlannedSpeed;
    e.etd = wp.m_ETD.IsValid() ? (long long)wp.m_ETD.GetTicks() : 0;
    e.tide_station = wp.m_TideStation;
    e.rr_count = wp.nrange_rings;
    e.rr_space = wp.RangeRingSpace;
    e.rr_units = wp.RangeRingSpaceUnits;
    e.rr_color = ColorToHex(wp.RangeRingColor);
    e.rr_show = wp.m_bShowWaypointRangeRings;
    if (wp.m_HyperlinkList)
    {
        Plugin_HyperlinkList::compatibility_iterator n =
            wp.m_HyperlinkList->GetFirst();
        while (n)
        {
            Plugin_Hyperlink *h = n->GetData();
            if (h)
            {
                HlEntry he;
                he.desc = h->DescrText;
                he.link = h->Link;
                he.type = h->Type;
                e.hyperlinks.push_back(he);
            }
            n = n->GetNext();
        }
    }
    return e;
}

// Build the sec-2A `mark` JSON for e, folding its canonical form into h. The hash
// uses %.6f lat/lon (a lossy change DETECTOR); the wire carries the full-precision
// double verbatim (sec 2A: "NO wire rounding, or coordinates drift every
// round-trip"). Reused for standalone marks[] and for embedded route vertices.
static nlohmann::json MarkToJson(const WpEntry &e, unsigned long long &h)
{
    // Change-detector canon: A fields + every carried B field, so an edit to any
    // carried field re-syncs (sec 12). %.6f for lat/lon (lossy detector only).
    wxString canon;
    canon << "M|" << e.guid << "|" << e.name << "|"
          << wxString::Format("%.6f", e.lat) << "|"
          << wxString::Format("%.6f", e.lon) << "|"
          << e.desc << "|" << e.icon << "|"
          << wxString::Format("%lld", e.created_ts) << "|"
          << (int)e.visible << (int)e.name_shown << (int)e.active << "|"
          << wxString::Format("%.6f", e.scamin) << (int)e.scamin_on << "|"
          << wxString::Format("%.6f", e.scamax) << "|"
          << wxString::Format("%.6f", e.arrival_radius) << "|"
          << wxString::Format("%.6f", e.planned_speed) << "|"
          << wxString::Format("%lld", e.etd) << "|" << e.tide_station << "|"
          << e.rr_count << wxString::Format("%.6f", e.rr_space) << e.rr_units
          << e.rr_color << (int)e.rr_show << "|";
    for (size_t k = 0; k < e.hyperlinks.size(); k++)
        canon << e.hyperlinks[k].desc << "\x1f" << e.hyperlinks[k].link << "\x1f"
              << e.hyperlinks[k].type << "\x1e";
    canon << "\n";
    FnvUpdate(h, canon);

    nlohmann::json m;
    m["guid"] = ToU8(e.guid);
    m["name"] = ToU8(e.name);
    m["lat"] = e.lat;   // full-precision double, not the %.6f hash form
    m["lon"] = e.lon;
    m["description"] = ToU8(e.desc);
    m["icon"] = ToU8(e.icon);
    m["created_ts"] = e.created_ts;
    // B superset (sec 2A extended mark)
    m["visible"] = e.visible;
    m["name_shown"] = e.name_shown;
    m["active"] = e.active;
    m["scamin"] = e.scamin;
    m["scamin_on"] = e.scamin_on;
    m["scamax"] = e.scamax;
    m["arrival_radius"] = e.arrival_radius;
    m["planned_speed"] = e.planned_speed;
    m["etd"] = e.etd;
    m["tide_station"] = ToU8(e.tide_station);
    nlohmann::json rr;
    rr["count"] = e.rr_count;
    rr["space"] = e.rr_space;
    rr["units"] = e.rr_units;
    rr["color"] = ToU8(e.rr_color);
    rr["show"] = e.rr_show;
    m["range_rings"] = std::move(rr);
    nlohmann::json hl = nlohmann::json::array();
    for (size_t k = 0; k < e.hyperlinks.size(); k++)
    {
        nlohmann::json o;
        o["desc"] = ToU8(e.hyperlinks[k].desc);
        o["link"] = ToU8(e.hyperlinks[k].link);
        o["type"] = ToU8(e.hyperlinks[k].type);
        hl.push_back(std::move(o));
    }
    m["hyperlinks"] = std::move(hl);
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

// Parse navMate's {ok, navmate_dt, ocpn_dt [, want_icons, lib_gen]} reply. Returns
// false if the required DT pair is malformed. The symbol-channel fields are OPTIONAL
// (absent until the hub ships sec 7): missing want_icons -> false, lib_gen -> 0.
static bool ParseView(const wxString &body, bool &ok, long long &navmate_dt,
                      long long &ocpn_dt, bool &want_icons, long long &lib_gen)
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
    want_icons = json_scalar(s, "want_icons", tok) && (tok == "true" || tok == "1");
    lib_gen = json_scalar(s, "lib_gen", tok) ? atoll(tok.c_str()) : 0;

    // protocol_version: the hub self-announces; absent == "1.0" baseline (upward-compat).
    // Soft floor - warn once if the hub is below our MAJOR, but keep functioning.
    std::string pv;
    if (json_scalar(s, "protocol_version", pv) && pv.size() >= 2 &&
        pv.front() == '"' && pv.back() == '"')
        pv = pv.substr(1, pv.size() - 2);
    if (pv.empty())
        pv = "1.0";
    static bool s_ver_warned = false;
    if (atoi(pv.c_str()) < atoi(OE_PROTOCOL_VERSION) && !s_ver_warned)
    {
        s_ver_warned = true;
        oeLog(0, 0, "WARNING: hub protocol_version %s below ours %s "
              "(soft floor - continuing)", pv.c_str(), OE_PROTOCOL_VERSION);
    }
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
      m_echo_baseline_hash(0),
      m_icons_ensured(false),
      m_lib_gen(0),
      m_need_icons_pull(false),
      m_want_icons(false),
      m_icons_sent(false)
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
    oeLog(0, 0, "Init: v%s host=%s debug=%d", PLUGIN_VERSION_FULL,
          static_cast<const char *>(m_host_port.mb_str()), m_debug_level);

    wxLogMessage("oESeries: Init - navMate spoke plugin loaded (v%s); "
                 "host=%s debug=%d",
                 PLUGIN_VERSION_FULL,
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
        PlugIn_Waypoint_ExV2 wp;
        if (GetSingleWaypointExV2(wguids[i], &wp) && wp.GetFSStatus())
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
               << route->m_StartString << "|" << route->m_EndString << "|"
               << route->m_Description << "|" << (int)route->m_isVisible
               << (int)route->m_isActive << "\n";
        FnvUpdate(h, rcanon);

        nlohmann::json rj;
        rj["guid"] = ToU8(route->m_GUID);
        rj["name"] = ToU8(route->m_NameString);
        rj["from"] = ToU8(route->m_StartString);   // B (sec 2A extended route)
        rj["to"] = ToU8(route->m_EndString);        // B
        rj["description"] = ToU8(route->m_Description);
        rj["visible"] = route->m_isVisible;         // B
        rj["active"] = route->m_isActive;           // B [R]

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
        tcanon << "T|" << trk->m_GUID << "|" << trk->m_NameString << "|"
               << trk->m_StartString << "|" << trk->m_EndString << "\n";
        FnvUpdate(h, tcanon);

        nlohmann::json tj;
        tj["guid"] = ToU8(trk->m_GUID);
        tj["name"] = ToU8(trk->m_NameString);
        tj["from"] = ToU8(trk->m_StartString);   // B (sec 2A extended track)
        tj["to"] = ToU8(trk->m_EndString);        // B

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

    // --- B fields (sec 6). Merge-on-apply: overlay only PRESENT fields. `active`
    // is read-only (up-only nav state), never applied. All are plain value writes
    // (no heap), so there is no cross-DLL ownership hazard. ---
    if (f.contains("visible") && f["visible"].is_boolean())
        wp.IsVisible = f["visible"].get<bool>();
    if (f.contains("name_shown") && f["name_shown"].is_boolean())
        wp.IsNameVisible = f["name_shown"].get<bool>();
    if (f.contains("scamin") && f["scamin"].is_number())
        wp.scamin = f["scamin"].get<double>();
    if (f.contains("scamin_on") && f["scamin_on"].is_boolean())
        wp.b_useScamin = f["scamin_on"].get<bool>();
    if (f.contains("scamax"))
    {
        // Trace to disambiguate the scamax round-trip (hub sends -> we apply ->
        // UpdateSingleWaypointExV2 SetScaMax -> readback). Tells us whether the
        // field arrived as a JSON number vs was dropped by the is_number guard.
        if (f["scamax"].is_number())
        {
            wp.scamax = f["scamax"].get<double>();
            oeLog(2, 2, "scamax field applied: %.1f", wp.scamax);
        }
        else
            oeLog(2, 2, "scamax present but non-number (type=%s) - skipped",
                  f["scamax"].type_name());
    }
    if (f.contains("arrival_radius") && f["arrival_radius"].is_number())
        wp.m_WaypointArrivalRadius = f["arrival_radius"].get<double>();
    if (f.contains("planned_speed") && f["planned_speed"].is_number())
        wp.m_PlannedSpeed = f["planned_speed"].get<double>();
    if (f.contains("etd") && f["etd"].is_number_integer())
    {
        long long ts = f["etd"].get<long long>();
        wp.m_ETD = (ts > 0) ? wxDateTime((time_t)ts) : wxInvalidDateTime;
    }
    if (f.contains("tide_station") && f["tide_station"].is_string())
        wp.m_TideStation =
            wxString::FromUTF8(f["tide_station"].get<std::string>().c_str());
    if (f.contains("range_rings") && f["range_rings"].is_object())
    {
        const nlohmann::json &rr = f["range_rings"];
        if (rr.contains("count") && rr["count"].is_number_integer())
            wp.nrange_rings = rr["count"].get<int>();
        if (rr.contains("space") && rr["space"].is_number())
            wp.RangeRingSpace = rr["space"].get<double>();
        if (rr.contains("units") && rr["units"].is_number_integer())
            wp.RangeRingSpaceUnits = rr["units"].get<int>();
        if (rr.contains("color") && rr["color"].is_string())
            wp.RangeRingColor =
                HexToColor(wxString::FromUTF8(rr["color"].get<std::string>().c_str()));
        if (rr.contains("show") && rr["show"].is_boolean())
            wp.m_bShowWaypointRangeRings = rr["show"].get<bool>();
    }
    // hyperlinks APPLY is bench-deferred: rebuilding m_HyperlinkList crosses the
    // plugin/OpenCPN heap boundary (Plugin_Hyperlink alloc/free ownership), which
    // must be validated on the bench before it runs live. EMIT works; a hyperlink
    // EDIT round-trip lands with S2 integration. Absent field -> live list preserved.
    if (f.contains("hyperlinks"))
        oeLog(2, 2, "hyperlinks apply deferred (bench) - emit-only for now");
}

// ---- S3 symbol channel (sec 7), all main-thread (model access) ----

// Direction A trip-wire (pin 5): lowercase-hex SHA-256 over the FOREIGN (non-nm:)
// icon-name set, sorted ascending by codepoint (UTF-8 byte sort == codepoint order),
// joined with a single '\n', no trailing newline. Names AS-IS (case-sensitive).
wxString oESeriesPi::ComputeIconHash()
{
    wxArrayString names = GetIconNameArray();
    std::vector<std::string> foreign;
    for (size_t i = 0; i < names.GetCount(); i++)
        if (!names[i].StartsWith(NM_ICON_PREFIX))
            foreign.push_back(std::string(names[i].ToUTF8().data()));
    std::sort(foreign.begin(), foreign.end());
    std::string joined;
    for (size_t i = 0; i < foreign.size(); i++)
    {
        if (i) joined += "\n";
        joined += foreign[i];
    }
    return wxString::FromUTF8(oe_sha256::hex(joined).c_str());
}

// Direction A payload (sec 7 / sec 2A): the FOREIGN icon set reported UP, now with
// 48x48 PNG rasters [PNG-only, protocol.md Turn 27/28].
//
// FindSystemWaypointIcon is NOT exported (dumpbin), so raster-by-name is impossible - but
// the source FILES are locatable off-disk. Two stores:
//   - stock markicons: GetpSharedDataLocation()/uidata/markicons/<file>.svg. Descriptive
//     stems map <name>.svg directly; the ~43 legacy short-name aliases need the table
//     below (transcribed from core WayPointmanGui::ProcessDefaultIcons, Release_5.12.4).
//   - user icons:  GetpPrivateApplicationDataLocation()/UserIcons/<name>.{svg,png,xpm}.
// A user file of the same name OVERRIDES the stock icon (core substitutes by name) and
// marks the entry builtin:false. Names with no locatable file (other-plugin in-memory
// injects) stay fmt:"none". All rasterizing runs on the MAIN thread (BuildPostBody is
// called from OnTimer), so wxBitmap/GetBitmapFromSVGFile are safe; want_icons is
// fetch-on-demand so this full sweep is occasional.

struct LegacyIcon { const char *key; const char *file; };
static const LegacyIcon LEGACY_ICONS[] = {
    { "empty",        "Symbol-Empty.svg" },
    { "triangle",     "Symbol-Triangle.svg" },
    { "activepoint",  "1st-Active-Waypoint.svg" },
    { "boarding",     "Marks-Boarding-Location.svg" },
    { "airplane",     "Hazard-Airplane.svg" },
    { "anchorage",    "1st-Anchorage.svg" },
    { "anchor",       "Symbol-Anchor2.svg" },
    { "boundary",     "Marks-Boundary.svg" },
    { "buoy1",        "Marks-Buoy-TypeA.svg" },
    { "buoy2",        "Marks-Buoy-TypeB.svg" },
    { "campfire",     "Activity-Campfire.svg" },
    { "camping",      "Activity-Camping.svg" },
    { "coral",        "Sea-Floor-Coral.svg" },
    { "fishhaven",    "Activity-Fishing.svg" },
    { "fishing",      "Activity-Fishing.svg" },
    { "fish",         "Activity-Fishing.svg" },
    { "float",        "Marks-Mooring-Buoy.svg" },
    { "food",         "Service-Food.svg" },
    { "greenlite",    "Marks-Light-Green.svg" },
    { "kelp",         "Sea-Floor-Sea-Weed.svg" },
    { "light",        "Marks-Light-TypeA.svg" },
    { "light1",       "Marks-Light-TypeB.svg" },
    { "litevessel",   "Marks-Light-Vessel.svg" },
    { "mob",          "1st-Man-Overboard.svg" },
    { "mooring",      "Marks-Mooring-Buoy.svg" },
    { "oilbuoy",      "Marks-Mooring-Buoy-Super.svg" },
    { "platform",     "Hazard-Oil-Platform.svg" },
    { "redgreenlite", "Marks-Light-Red-Green.svg" },
    { "redlite",      "Marks-Light-Red.svg" },
    { "rock1",        "Hazard-Rock-Exposed.svg" },
    { "rock2",        "Hazard-Rock-Awash.svg" },
    { "sand",         "Hazard-Sandbar.svg" },
    { "scuba",        "Activity-Diving-Scuba-Flag.svg" },
    { "shoal",        "Hazard-Sandbar.svg" },
    { "snag",         "Hazard-Snag.svg" },
    { "square",       "Symbol-Square.svg" },
    { "diamond",      "1st-Diamond.svg" },
    { "circle",       "Symbol-Circle.svg" },
    { "wreck1",       "Hazard-Wreck1.svg" },
    { "wreck2",       "Hazard-Wreck2.svg" },
    { "xmblue",       "Symbol-X-Small-Blue.svg" },
    { "xmgreen",      "Symbol-X-Small-Green.svg" },
    { "xmred",        "Symbol-X-Small-Red.svg" },
};

// <base>/<sub>/ with a guaranteed trailing separator; empty if base is null/empty.
static wxString IconDirWith(const wxString *base, const char *sub)
{
    if (!base || base->IsEmpty()) return wxString();
    wxString d = *base;
    if (d.Last() != wxFILE_SEP_PATH) d += wxFILE_SEP_PATH;
    d += sub; d += wxFILE_SEP_PATH;
    return d;
}

// A user-supplied icon file for `name` (svg/png/xpm), or empty. Presence => builtin:false.
static wxString UserIconFile(const wxString &name)
{
    wxString dir = IconDirWith(GetpPrivateApplicationDataLocation(), "UserIcons");
    if (dir.IsEmpty()) return wxString();
    static const char *const kExt[] = { ".svg", ".png", ".xpm" };
    for (const char *e : kExt)
    {
        wxString f = dir + name + e;
        if (wxFileName::FileExists(f)) return f;
    }
    return wxString();
}

// The stock markicon SVG file for `name` (legacy alias or descriptive stem), or empty.
static wxString StockIconFile(const wxString &name)
{
    wxString dir = IconDirWith(GetpSharedDataLocation(), "uidata");
    if (dir.IsEmpty()) return wxString();
    dir += "markicons"; dir += wxFILE_SEP_PATH;
    for (const LegacyIcon &li : LEGACY_ICONS)
        if (name == li.key)
        {
            wxString f = dir + li.file;
            if (wxFileName::FileExists(f)) return f;
        }
    wxString stem = dir + name + ".svg";
    if (wxFileName::FileExists(stem)) return stem;
    return wxString();
}

// Content-fill an image into a 48x48 RGBA box: crop to the alpha CONTENT bounding box,
// then scale that content (aspect-preserved) to fit a 44px target - a small uniform
// breathing margin - centered on a transparent 48x48 canvas. Stock markicon SVGs have a
// glyph small inside a padded viewBox; rendering large + cropping here puts the icon's
// resolution in the cell, so the consumer only ever DOWNSCALES (crisp), never upscales.
static wxImage ContentFill48(wxImage img)
{
    const int BOX = 48;
    const int FIT = 44;   // content target within the 48 box (breathing margin)
    int w = img.GetWidth(), h = img.GetHeight();
    if (w <= 0 || h <= 0) return img;
    if (!img.HasAlpha()) img.InitAlpha();

    // Tight alpha content bounding box.
    const unsigned char *a = img.GetAlpha();
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (a[y * w + x] != 0)
            {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }

    wxImage canvas(BOX, BOX);
    canvas.InitAlpha();
    unsigned char *ca = canvas.GetAlpha();
    for (int k = 0; k < BOX * BOX; k++) ca[k] = 0;   // transparent
    if (maxx < minx || maxy < miny)
        return canvas;   // fully transparent source

    int cw = maxx - minx + 1, ch = maxy - miny + 1;
    wxImage content = img.GetSubImage(wxRect(minx, miny, cw, ch));
    double s = (double)FIT / (cw >= ch ? cw : ch);
    int nw = wxMax(1, (int)(cw * s + 0.5));
    int nh = wxMax(1, (int)(ch * s + 0.5));
    content = content.Scale(nw, nh, wxIMAGE_QUALITY_HIGH);
    if (!content.HasAlpha()) content.InitAlpha();
    canvas.Paste(content, (BOX - nw) / 2, (BOX - nh) / 2);
    return canvas;
}

// Rasterize an icon source file to a 48x48 RGBA PNG; fill b64 + sha256. false on failure.
static bool RasterizeIconPng(const wxString &file, std::string &b64, std::string &hash)
{
    wxImage img;
    if (wxFileName(file).GetExt().Lower() == "svg")
    {
        // Render LARGE (not 48) so the glyph inside the padded viewBox has real pixels;
        // ContentFill48 then crops to content and downscales to fit 48 - crisp, no upscale.
        wxBitmap bmp = GetBitmapFromSVGFile(file, 256, 256);
        if (!bmp.IsOk()) return false;
        img = bmp.ConvertToImage();
    }
    else if (!img.LoadFile(file))
        return false;
    if (!img.IsOk()) return false;
    img = ContentFill48(img);
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
        wxImage::AddHandler(new wxPNGHandler);
    wxMemoryOutputStream mos;
    if (!img.SaveFile(mos, wxBITMAP_TYPE_PNG)) return false;
    size_t len = (size_t)mos.GetLength();
    if (len == 0) return false;
    std::vector<unsigned char> buf(len);
    mos.CopyTo(buf.data(), len);
    b64 = std::string(wxBase64Encode(buf.data(), len).ToUTF8().data());
    hash = oe_sha256::hex(std::string((const char *)buf.data(), len));
    return true;
}

wxString oESeriesPi::BuildOcpnIcons()
{
    wxArrayString names = GetIconNameArray();
    nlohmann::json arr = nlohmann::json::array();
    int n_png = 0, n_none = 0;
    for (size_t i = 0; i < names.GetCount(); i++)
    {
        if (names[i].StartsWith(NM_ICON_PREFIX))
            continue;
        const wxString &name = names[i];

        wxString user = UserIconFile(name);
        bool builtin = user.IsEmpty();
        wxString file = builtin ? StockIconFile(name) : user;

        nlohmann::json o;
        o["name"] = ToU8(name);
        o["description"] = ToU8(name);   // api-20 exposes no per-icon description
        o["builtin"] = builtin;

        std::string b64, hash;
        bool ok = false;
        try { ok = !file.IsEmpty() && RasterizeIconPng(file, b64, hash); }
        catch (...) { ok = false; }   // one bad icon must never zero the whole payload
        if (ok)
        {
            o["fmt"] = "png";
            o["data_b64"] = b64;
            o["byte_hash"] = hash;
            n_png++;
        }
        else
        {
            o["fmt"] = "none";           // no locatable source (e.g. other-plugin inject)
            o["data_b64"] = std::string();
            o["byte_hash"] = std::string();
            n_none++;
        }
        arr.push_back(std::move(o));
    }
    oeLog(1, 0, "BuildOcpnIcons: %d png, %d names-only", n_png, n_none);
    return wxString::FromUTF8(arr.dump(-1, ' ', true).c_str());
}

// Direction B (pin 2/6): parse a ?icons=1 body {lib_gen, nm_icons:[...]} and register
// each via AddCustomWaypointIcon (session-only registration, sec 7). Returns count
// registered, or <0 on a parse failure; lib_gen_out carries the body's lib_gen.
int oESeriesPi::RegisterNmIcons(const wxString &body, long long &lib_gen_out)
{
    lib_gen_out = 0;
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(std::string(body.ToUTF8().data()));
    }
    catch (...)
    {
        return -1;
    }
    if (j.contains("lib_gen") && j["lib_gen"].is_number_integer())
        lib_gen_out = j["lib_gen"].get<long long>();
    if (!j.contains("nm_icons") || !j["nm_icons"].is_array())
        return -1;
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
        wxImage::AddHandler(new wxPNGHandler);
    int n = 0;
    for (const nlohmann::json &ic : j["nm_icons"])
    {
        std::string key = ic.value("key", std::string());
        std::string b64 = ic.value("data_b64", std::string());
        std::string desc = ic.value("description", std::string());
        std::string fmt = ic.value("fmt", std::string("png"));
        if (key.empty() || b64.empty())
            continue;
        // Raster PNG is the v1 floor (pin 2/3). fmt "svg" (GetBitmapFromSVGFile) is
        // deferred - it needs a temp file path, not a byte stream.
        if (fmt != "png")
            continue;
        wxMemoryBuffer raw = wxBase64Decode(wxString::FromUTF8(b64.c_str()));
        wxMemoryInputStream mis(raw.GetData(), raw.GetDataLen());
        wxImage img;
        if (!img.LoadFile(mis, wxBITMAP_TYPE_PNG))
            continue;
        wxBitmap bmp(img);
        if (!bmp.IsOk())
            continue;
        wxString wkey = wxString::FromUTF8(key.c_str());
        wxString wdesc = wxString::FromUTF8(desc.c_str());
        if (AddCustomWaypointIcon(&bmp, wkey, wdesc))
            n++;
    }
    oeLog(1, 0, "registered %d nm: icon(s) (lib_gen=%lld)", n, lib_gen_out);
    return n;
}

// Ordering gate (sec 7): true if a command's `icon` (or any embedded route-point
// mark icon) is an nm: key, which must not be applied until icons_ensured.
static bool JsonIconIsNm(const nlohmann::json &f)
{
    if (f.is_object() && f.contains("icon") && f["icon"].is_string())
        return f["icon"].get<std::string>().rfind(NM_ICON_PREFIX, 0) == 0;
    return false;
}
static bool CommandRefsNmIcon(const nlohmann::json &cmd)
{
    if (!cmd.contains("fields") || !cmd["fields"].is_object())
        return false;
    const nlohmann::json &f = cmd["fields"];
    if (JsonIconIsNm(f))
        return true;
    if (f.contains("points") && f["points"].is_array())
        for (const nlohmann::json &pt : f["points"])
            if (pt.contains("mark") && JsonIconIsNm(pt["mark"]))
                return true;
    return false;
}

// Assemble the full POST body from the cached inventory arrays + pending results,
// plus the symbol-channel report (icon_hash always; ocpn_icons only when the hub
// asked, sec 7 Direction A).
wxString oESeriesPi::BuildPostBody()
{
    wxString icons = wxString("[]");
    if (m_want_icons)
    {
        icons = BuildOcpnIcons();
        m_icons_sent = true;   // delivered for this want_icons request (rising-edge gate)
    }
    wxString b;
    b << "{\"protocol_version\":\"" << OE_PROTOCOL_VERSION << "\""
      << ",\"dt\":" << DtStr(m_dt_ocpn)
      << ",\"marks\":"   << (m_marks_json.IsEmpty()  ? wxString("[]") : m_marks_json)
      << ",\"routes\":"  << (m_routes_json.IsEmpty() ? wxString("[]") : m_routes_json)
      << ",\"tracks\":"  << (m_tracks_json.IsEmpty() ? wxString("[]") : m_tracks_json)
      << ",\"results\":"
      << (m_pending_results.IsEmpty() ? wxString("[]") : m_pending_results)
      << ",\"icon_hash\":\"" << ComputeIconHash() << "\""
      << ",\"icons_ensured\":" << (m_icons_ensured ? "true" : "false")
      << ",\"ocpn_icons\":" << icons
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

// True if a track with this GUID already exists. An existing guid routes an
// add/update through UpdatePlugInTrack (upsert) instead of AddPlugInTrack - the
// latter would DUPLICATE the track (a re-GET of a not-yet-retired batch re-applies
// it). Tracks ARE editable in place (protocol sec 11): UpdatePlugInTrack is
// internally delete+reinsert, GUID-preserving, rebuilding from the passed points.
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
    route.m_isVisible = true;   // default; overridden by the `visible` B field below
    if (fields.contains("name") && fields["name"].is_string())
        route.m_NameString =
            wxString::FromUTF8(fields["name"].get<std::string>().c_str());
    if (fields.contains("description") && fields["description"].is_string())
        route.m_Description =
            wxString::FromUTF8(fields["description"].get<std::string>().c_str());
    // B fields (sec 6). `active` is read-only, never applied.
    if (fields.contains("from") && fields["from"].is_string())
        route.m_StartString =
            wxString::FromUTF8(fields["from"].get<std::string>().c_str());
    if (fields.contains("to") && fields["to"].is_string())
        route.m_EndString =
            wxString::FromUTF8(fields["to"].get<std::string>().c_str());
    if (fields.contains("visible") && fields["visible"].is_boolean())
        route.m_isVisible = fields["visible"].get<bool>();

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
// add-or-update it. Same copy-semantics/cleanup discipline as routes.
//
// exists -> UpdatePlugInTrack, else AddPlugInTrack. UpdatePlugInTrack is internally
// DeleteTrack + AddPlugInTrack (OpenCPN 5.12.4, source-verified): it PRESERVES the
// GUID but REBUILDS the track from the passed pWaypointList - so we MUST pass the
// FULL point list (protocol sec 8/11), which the inbound track object always
// carries. This is the fix for the winOCPN track-rename bug (a rename arrives as an
// add-of-existing-GUID that previously no-op'd).
static bool ApplyTrackObject(const wxString &guid, const nlohmann::json &fields,
                             bool exists, wxString &err)
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
    if (fields.contains("from") && fields["from"].is_string())   // B
        track.m_StartString =
            wxString::FromUTF8(fields["from"].get<std::string>().c_str());
    if (fields.contains("to") && fields["to"].is_string())        // B
        track.m_EndString =
            wxString::FromUTF8(fields["to"].get<std::string>().c_str());

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
    const char *call = exists ? "UpdatePlugInTrack" : "AddPlugInTrack";
    oeLog(2, 2, "track: pre-%s, %d points", call, (int)lst->GetCount());
    bool ok = exists ? UpdatePlugInTrack(&track) : AddPlugInTrack(&track, true);
    oeLog(2, 2, "track: post-%s ok=%d", call, (int)ok);
    // Free the waypoint DATA we allocated (the dtor won't - see below), but do
    // NOT `delete lst` or null track.pWaypointList. ~PlugIn_Track() (core,
    // ocpn_plugin_gui.cpp) unconditionally runs pWaypointList->DeleteContents(false)
    // + Clear() + delete pWaypointList with NO null guard. Nulling the list (as the
    // route path safely can - ~PlugIn_Route_ExV2 IS null-guarded) makes that dtor
    // deref nullptr and CRASH OpenCPN when the stack `track` unwinds. DeleteContents(
    // false) tells the dtor NOT to free the waypoint data (we own it, freed just
    // below); we hand it the still-valid container to Clear() + delete. Net: each
    // waypoint freed once (here), the list container freed once (dtor) - no
    // double-free, no null-deref, no leak.
    for (Plugin_WaypointList::compatibility_iterator n = lst->GetFirst(); n;
         n = n->GetNext())
        delete n->GetData();
    oeLog(2, 2, "track: cleanup done");
    if (!ok)
        err = exists ? "UpdatePlugInTrack failed" : "AddPlugInTrack failed";
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
                d["version"] = PLUGIN_VERSION_FULL;   // internal X.Y.Z.NNN of the running build
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
                    // B fields (sec 6) so the hub can OBSERVE they applied, not
                    // just infer it - GetRouteExV2_Plugin fills these from the model
                    // (core: m_RouteStartString/m_RouteEndString/IsVisible).
                    d["from"] = ToU8(route->m_StartString);
                    d["to"] = ToU8(route->m_EndString);
                    d["visible"] = route->m_isVisible;
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
                    // B fields (sec 6): from/to so the hub can observe track
                    // rename/from-to edits (GetTrack_Plugin fills these from the
                    // model's m_TrackStartString/m_TrackEndString).
                    d["from"] = ToU8(trk->m_StartString);
                    d["to"] = ToU8(trk->m_EndString);
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

        // Ordering gate (sec 7): hold any command whose icon (or an embedded route
        // point's mark icon) is an nm: key until the nm: library is registered, so a
        // navMate-origin mark never renders as a fallback glyph. The hub retries;
        // once icons_ensured the apply proceeds on a later GET.
        if ((op == "add" || op == "update") && !m_icons_ensured &&
            CommandRefsNmIcon(cmd))
        {
            r["error"] = "nm: icon not registered yet (ordering gate)";
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
                    // scamax probe: log the struct going IN, then re-read the model
                    // immediately after, to localize the scamax=0 puzzle to core
                    // SetScaMax vs a later reload (both scamax + scamin for context).
                    oeLog(2, 2, "scamax pre-Update: wp.scamax=%.1f wp.scamin=%.1f",
                          wp.scamax, wp.scamin);
                    r["ok"] = UpdateSingleWaypointExV2(&wp);
                    PlugIn_Waypoint_ExV2 rb;
                    if (GetSingleWaypointExV2(g, &rb))
                        oeLog(2, 2, "scamax post-Update model: scamax=%.1f scamin=%.1f",
                              rb.scamax, rb.scamin);
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
                    bool ok = false;
                    try   // degrade a bad apply to ok:false, never crash the loop
                    {     // (NOTE: catches C++ exceptions only, not SEH/AVs)
                        ok = ApplyRouteObject(
                            g, cmd.value("fields", nlohmann::json::object()),
                            exists, err);
                    }
                    catch (const std::exception &e)
                    {
                        err = wxString::FromUTF8(e.what());
                        oeLog(0, 1, "route apply threw: %s", e.what());
                    }
                    catch (...)
                    {
                        err = "route apply threw (unknown)";
                        oeLog(0, 1, "route apply threw (unknown)");
                    }
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
            else if (op == "add" || op == "update")
            {
                bool exists = TrackExists(g);
                if (!exists && op == "update")
                {
                    // update-of-vanished -> err; the hub re-drives as add (sec 8).
                    r["error"] = "update of missing track GUID";
                }
                else
                {
                    // add-of-existing = upsert -> UpdatePlugInTrack (the rename fix,
                    // sec 8/11); add-of-new -> AddPlugInTrack. Both pass full points,
                    // and both are idempotent under re-GET-before-retire.
                    wxString err;
                    bool ok = false;
                    try   // degrade a bad apply to ok:false, never crash the loop
                    {     // (NOTE: catches C++ exceptions only, not SEH/AVs)
                        ok = ApplyTrackObject(
                            g, cmd.value("fields", nlohmann::json::object()),
                            exists, err);
                    }
                    catch (const std::exception &e)
                    {
                        err = wxString::FromUTF8(e.what());
                        oeLog(0, 1, "track apply threw: %s", e.what());
                    }
                    catch (...)
                    {
                        err = "track apply threw (unknown)";
                        oeLog(0, 1, "track apply threw (unknown)");
                    }
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
            if (res.tag == TAG_ICONS)
            {
                // ?icons=1 reply {lib_gen, nm_icons[]} - NOT the view shape. Register
                // the nm: set; do not run ParseView/ApplyGetView on this body.
                long long lg = 0;
                if (RegisterNmIcons(res.body, lg) >= 0)
                {
                    m_icons_ensured = true;
                    m_lib_gen = lg;
                    m_need_icons_pull = false;
                }
            }
            else
            {
            bool ok = false;
            long long ndt = 0, odt = 0;
            bool want_icons = false;
            long long lib_gen = 0;
            if (ParseView(res.body, ok, ndt, odt, want_icons, lib_gen))
            {
                m_navmate_dt = ndt;
                // Symbol channel (sec 7): remember want_icons for the next POST; a
                // lib_gen advance vs what we registered triggers a ?icons=1 pull.
                // lib_gen==0 => hub has no symbol channel yet (no-op).
                m_want_icons = want_icons;
                if (lib_gen > 0 && lib_gen != m_lib_gen)
                    m_need_icons_pull = true;
                bool matched = ((unsigned long long)odt == m_dt_ocpn);
                m_want_post = !matched;
                // Fetch-on-demand: a want_icons request must FORCE a POST even when the
                // inventory is stable, or the icon payload never rides (only a DT change
                // sets want_post otherwise). Send once per want_icons rising edge.
                if (m_want_icons && !m_icons_sent)
                    m_want_post = true;
                if (!m_want_icons)
                    m_icons_sent = false;
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
            }   // end else (non-TAG_ICONS reply)
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
        // Priority: pull the nm: library (Direction B) before POSTing/GETing, so the
        // ordering gate clears promptly on connect / lib_gen advance (sec 7).
        if (m_need_icons_pull)
            m_http->Submit(TAG_ICONS, "GET", host, port, OCPN_ICONS_PATH,
                           wxEmptyString);
        else if (m_want_post && m_have_inventory)
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
    // Empty so OpenCPN renders a clean "0.1.0" (semantic_vers.cpp appends "+<build>"
    // only when non-empty). The internal X.Y.NNN build stamp lives in the log/diag,
    // not in OpenCPN's version display.
    return "";
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
