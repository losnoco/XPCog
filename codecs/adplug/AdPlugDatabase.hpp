// Installing AdPlug's song database. See AdPlugDatabase.cpp for why it is
// compiled in rather than shipped as a file.

#pragma once

namespace xpcog::adplug {

/// Loads the compiled-in database and hands it to CAdPlug, once per process.
/// Safe to call from any thread and from every open(); later calls do nothing.
void installDatabase();

}  // namespace xpcog::adplug
