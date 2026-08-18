// Open a stream by typing its address.
//
// Port of Cog's OpenURLPanel (ThirdParty/OpenURLPanel), which is an editable
// combo box over the fifteen most recently used URLs. The history is the point
// rather than a flourish: a radio station is a URL nobody remembers and everyone
// returns to, and without it the feature is "paste it again every time".
//
// A QDialog rather than Cog's window-modal sheet, because a sheet is an AppKit
// shape with no cross-platform equivalent -- Qt would render it as a plain modal
// dialog on Windows and Linux regardless.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <QDialog>
#include <QString>

class QComboBox;

namespace xpcog::app {

class OpenUrlDialog : public QDialog {
    Q_OBJECT

public:
    explicit OpenUrlDialog(Settings& settings, QWidget* parent = nullptr);

    /// The address entered, trimmed. Only meaningful after exec() returns
    /// Accepted, at which point it is guaranteed to parse.
    [[nodiscard]] QString url() const;

    /// Splits the stored history newest-last. Exposed for tests.
    [[nodiscard]] static QStringList historyFrom(const std::string& stored);

    /// Folds `url` into `history` and returns the result, newest last and capped.
    /// A repeat moves to the end rather than being added twice, so re-opening a
    /// station keeps it to hand instead of filling the list with itself.
    [[nodiscard]] static QStringList withUrl(QStringList history, const QString& url);

private:
    void accept() override;

    Settings&  settings_;
    QComboBox* input_ = nullptr;
};

}  // namespace xpcog::app
