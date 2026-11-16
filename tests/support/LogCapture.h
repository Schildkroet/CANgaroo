#pragma once

#include <QStringList>

// core/Log replacement for this test: keeps what the driver logged so tests can
// check that a problem was reported. Thread safe: drivers log from their
// reader threads.
namespace logcapture
{

[[nodiscard]] QStringList warnings();
[[nodiscard]] QStringList errors();
void clear();

} // namespace logcapture
