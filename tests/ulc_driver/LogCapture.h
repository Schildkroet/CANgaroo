#pragma once

#include <QStringList>

// core/Log replacement for this test: keeps what the driver logged so tests can
// check that a problem was reported. Single threaded, like the tests.
namespace logcapture
{

[[nodiscard]] const QStringList &warnings();
[[nodiscard]] const QStringList &errors();
void clear();

} // namespace logcapture
