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

#ifndef OESERIES_LOG_H_
#define OESERIES_LOG_H_

// Leveled logger mirroring navMate's Perl display(level, indent, msg): a line is
// written to the plugin's own log file only when 'level' <= g_oeseries_debug_level
// (the live prefs value). Timestamped, flushed each write, mutex-guarded so the
// main thread and the HTTP worker can both call it. printf-style C args only
// (%s expects const char*, e.g. wxString via .mb_str()).

// Open the log file in the plugin private data dir. Call once from Init().
void oeLogInit();

// Close the log file. Call from DeInit().
void oeLogClose();

// Write a leveled, indented, timestamped line (if level <= global debug level).
void oeLog(int level, int indent, const char *fmt, ...);

#endif   // OESERIES_LOG_H_
