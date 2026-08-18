#include "OpenUrlDialog.hpp"

#include "xpcog/core/Url.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace xpcog::app {
namespace {

/// Cog's kMaximumURLs.
constexpr int kMaxHistory = 15;

}  // namespace

OpenUrlDialog::OpenUrlDialog(Settings& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("Open URL"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Address of a stream or file:"), this));

    input_ = new QComboBox(this);
    input_->setEditable(true);
    // Qt would otherwise offer the history as an inline completion, which fights
    // typing a new address that shares a prefix with an old one.
    input_->setInsertPolicy(QComboBox::NoInsert);
    input_->addItems(historyFrom(settings_.UrlHistory()));
    // Newest is last, so that is what should be showing.
    input_->setCurrentIndex(input_->count() - 1);
    input_->setMinimumWidth(420);
    layout->addWidget(input_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Open"));
    connect(buttons, &QDialogButtonBox::accepted, this, &OpenUrlDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    input_->setFocus();
}

QString OpenUrlDialog::url() const {
    return input_->currentText().trimmed();
}

QStringList OpenUrlDialog::historyFrom(const std::string& stored) {
    QStringList entries;
    for (const QString& line :
         QString::fromStdString(stored).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            entries.append(trimmed);
        }
    }
    return entries;
}

QStringList OpenUrlDialog::withUrl(QStringList history, const QString& url) {
    history.removeAll(url);
    history.append(url);
    while (history.size() > kMaxHistory) {
        history.removeFirst();
    }
    return history;
}

void OpenUrlDialog::accept() {
    const QString text = url();
    if (text.isEmpty()) {
        return;
    }

    // Validated against the same parser that will be asked to open it, rather
    // than against QUrl. QUrl accepts a bare path as a relative URL, so a
    // mistyped address would be accepted here and fail silently later with
    // nothing to point at.
    if (!Url::parse(text.toStdString()).has_value()) {
        QMessageBox::warning(
            this, tr("Invalid URL"),
            tr("“%1” is not an address XPCog can open. It needs a scheme, "
               "such as https:// or file://.")
                .arg(text));
        return;
    }

    settings_.setUrlHistory(
        withUrl(historyFrom(settings_.UrlHistory()), text).join(QLatin1Char('\n'))
            .toStdString());

    QDialog::accept();
}

}  // namespace xpcog::app
