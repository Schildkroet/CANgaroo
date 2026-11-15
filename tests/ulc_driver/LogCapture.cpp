/*

  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

// Like tests/support/LogStub.cpp, but records warnings and errors.

#include "LogCapture.h"

#include "core/Log.h"

namespace
{
QStringList g_warnings;
QStringList g_errors;
} // namespace

const QStringList &logcapture::warnings() { return g_warnings; }
const QStringList &logcapture::errors() { return g_errors; }

void logcapture::clear()
{
    g_warnings.clear();
    g_errors.clear();
}

void log_msg(const QDateTime, const log_level_t, const QString) {}
void log_msg(const log_level_t, const QString) {}

void log_debug(const QString) {}
void log_info(const QString) {}
void log_warning(const QString msg) { g_warnings.append(msg); }
void log_error(const QString msg) { g_errors.append(msg); }
void log_critical(const QString msg) { g_errors.append(msg); }
void log_fatal(const QString msg) { g_errors.append(msg); }
