// About XPCog.
//
// More than a version number, because this is a GPL-2.0 derivative work of Cog
// that links a dozen third-party libraries. Saying so in the application is the
// honest place for it -- a README nobody opens is not where attribution belongs,
// and for LGPL components it is a licence obligation rather than a courtesy.
//
// The codec list is read from the registry rather than written out, so a build
// with codecs disabled says so instead of claiming formats it cannot play.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"

#include <QDialog>

namespace xpcog::app {

class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(const PluginRegistry& registry, QWidget* parent = nullptr);
};

}  // namespace xpcog::app
