#include "ActionRegistry.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QActionGroup>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>

namespace xpcog::app {
namespace {

/// The context the menu titles below are recorded under.
///
/// It has to be named, because the table is at namespace scope where there is
/// no class for tr() to take a context from -- and it must be used on the
/// lookup side too. Calling tr() there instead would look in
/// "xpcog::app::ActionRegistry", which is a different context and always a
/// miss, so the menu bar would stay English in every language while every menu
/// *item* translated.
constexpr auto kMenuContext = "ActionRegistry";

/// The menu structure, declaratively. Adding a command is one row here plus one
/// row in the constructor, rather than an edit to a XIB whose diff is unreadable.
struct MenuItem {
    const char* menu;  ///< nullptr continues the previous menu
    ActionId    id;
    bool        separatorBefore = false;
};

constexpr MenuItem kMenuLayout[] = {
    {QT_TRANSLATE_NOOP("ActionRegistry", "&File"), ActionId::FileOpen},
    {nullptr, ActionId::FileOpenFolder},
    {nullptr, ActionId::FileSavePlaylist, true},
    {nullptr, ActionId::FilePreferences, true},
    {nullptr, ActionId::FileQuit, true},

    {QT_TRANSLATE_NOOP("ActionRegistry", "&View"), ActionId::ViewFileTree},
    {nullptr, ActionId::ViewSpectrum},

    {QT_TRANSLATE_NOOP("ActionRegistry", "&Edit"), ActionId::EditUndo},
    {nullptr, ActionId::EditRedo},
    {nullptr, ActionId::EditSelectAll, true},
    {nullptr, ActionId::EditRemove},
    {nullptr, ActionId::EditRandomize, true},

    {QT_TRANSLATE_NOOP("ActionRegistry", "&Playback"), ActionId::PlaybackPlayPause},
    {nullptr, ActionId::PlaybackStop},
    {nullptr, ActionId::PlaybackPrevious, true},
    {nullptr, ActionId::PlaybackNext},
    {nullptr, ActionId::PlaybackEnqueue, true},

    {QT_TRANSLATE_NOOP("ActionRegistry", "&Order"), ActionId::OrderRepeatNone},
    {nullptr, ActionId::OrderRepeatOne},
    {nullptr, ActionId::OrderRepeatAlbum},
    {nullptr, ActionId::OrderRepeatAll},
    {nullptr, ActionId::OrderShuffleOff, true},
    {nullptr, ActionId::OrderShuffleAlbums},
    {nullptr, ActionId::OrderShuffleAll},

    {QT_TRANSLATE_NOOP("ActionRegistry", "&Help"), ActionId::HelpAbout},
    {nullptr, ActionId::HelpAboutQt},
};

constexpr ActionId kToolBarLayout[] = {
    ActionId::PlaybackPrevious,
    ActionId::PlaybackPlayPause,
    ActionId::PlaybackStop,
    ActionId::PlaybackNext,
};

}  // namespace

ActionRegistry::ActionRegistry(QObject* parent) : QObject(parent) {
    add(ActionId::FileOpen, tr("&Open Files…"), QKeySequence::Open);
    add(ActionId::FileOpenFolder, tr("Open &Folder…"),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    add(ActionId::FileSavePlaylist, tr("&Save Playlist…"), QKeySequence::Save);
    // Qt moves this to the application menu on macOS, where it belongs.
    add(ActionId::FilePreferences, tr("&Preferences…"), QKeySequence::Preferences);
    add(ActionId::FileQuit, tr("&Quit"), QKeySequence::Quit);

    QAction* tree = add(ActionId::ViewFileTree, tr("&File Browser"),
                        QKeySequence(Qt::CTRL | Qt::Key_B));
    tree->setCheckable(true);
    tree->setChecked(true);

    QAction* spectrum = add(ActionId::ViewSpectrum, tr("&Spectrum"),
                            QKeySequence(Qt::CTRL | Qt::Key_U));
    spectrum->setCheckable(true);
    spectrum->setChecked(true);

    // Undo and Redo are enabled by the undo stack, not here: an always-enabled
    // Undo that does nothing is worse than a greyed-out one.
    add(ActionId::EditUndo, tr("&Undo"), QKeySequence::Undo)->setEnabled(false);
    add(ActionId::EditRedo, tr("&Redo"), QKeySequence::Redo)->setEnabled(false);
    add(ActionId::EditSelectAll, tr("Select &All"), QKeySequence::SelectAll);
    add(ActionId::EditRemove, tr("&Remove from Playlist"), QKeySequence::Delete);
    add(ActionId::EditRandomize, tr("Randomi&ze Playlist"));

    add(ActionId::PlaybackPlayPause, tr("&Play/Pause"), QKeySequence(Qt::Key_Space));
    add(ActionId::PlaybackStop, tr("&Stop"), QKeySequence(Qt::CTRL | Qt::Key_Period));
    add(ActionId::PlaybackNext, tr("&Next"), QKeySequence(Qt::CTRL | Qt::Key_Right));
    add(ActionId::PlaybackPrevious, tr("Pre&vious"), QKeySequence(Qt::CTRL | Qt::Key_Left));
    add(ActionId::PlaybackEnqueue, tr("Add to &Queue"), QKeySequence(Qt::Key_Q));

    // Repeat and shuffle are exclusive sets, so they are checkable and grouped.
    // Cog drives these from an NSPopUpButton plus four NSValueTransformers; a
    // QActionGroup is the same idea with the transformers deleted.
    auto* repeat = new QActionGroup(this);
    for (const auto& [id, label] :
         {std::pair{ActionId::OrderRepeatNone, tr("Repeat: &Off")},
          std::pair{ActionId::OrderRepeatOne, tr("Repeat: &One")},
          std::pair{ActionId::OrderRepeatAlbum, tr("Repeat: &Album")},
          std::pair{ActionId::OrderRepeatAll, tr("Repeat: A&ll")}}) {
        QAction* item = add(id, label);
        item->setCheckable(true);
        repeat->addAction(item);
    }

    // Menu roles rather than relying on Qt's text heuristics: on macOS both of
    // these belong in the application menu, and the heuristic only matches
    // English text. Naming the role keeps them there once translated.
    QAction* about = add(ActionId::HelpAbout, tr("&About XPCog"));
    about->setMenuRole(QAction::AboutRole);
    QAction* aboutQt = add(ActionId::HelpAboutQt, tr("About &Qt"));
    aboutQt->setMenuRole(QAction::AboutQtRole);

    auto* shuffle = new QActionGroup(this);
    for (const auto& [id, label] :
         {std::pair{ActionId::OrderShuffleOff, tr("Shuffle: O&ff")},
          std::pair{ActionId::OrderShuffleAlbums, tr("Shuffle: Al&bums")},
          std::pair{ActionId::OrderShuffleAll, tr("Shuffle: A&ll Tracks")}}) {
        QAction* item = add(id, label);
        item->setCheckable(true);
        shuffle->addAction(item);
    }
}

QAction* ActionRegistry::add(ActionId id, const QString& text,
                             const QKeySequence& shortcut) {
    auto* item = new QAction(text, this);
    if (!shortcut.isEmpty()) {
        item->setShortcut(shortcut);
    }
    actions_.insert(static_cast<int>(id), item);
    return item;
}

QAction* ActionRegistry::action(ActionId id) const {
    return actions_.value(static_cast<int>(id), nullptr);
}

void ActionRegistry::populateMenuBar(QMenuBar* bar) const {
    QMenu* current = nullptr;
    for (const MenuItem& item : kMenuLayout) {
        if (item.menu != nullptr) {
            current = bar->addMenu(QCoreApplication::translate(kMenuContext, item.menu));
        }
        if (current == nullptr) {
            continue;
        }
        if (item.separatorBefore) {
            current->addSeparator();
        }
        if (QAction* command = action(item.id); command != nullptr) {
            current->addAction(command);
        }
    }
}

void ActionRegistry::populateToolBar(QToolBar* bar) const {
    for (const ActionId id : kToolBarLayout) {
        if (QAction* command = action(id); command != nullptr) {
            bar->addAction(command);
        }
    }
}

}  // namespace xpcog::app
