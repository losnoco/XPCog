#include "xpcog/platform/Notifier.hpp"

namespace xpcog::platform {
namespace {

class NullNotifier final : public Notifier {
public:
    bool isAvailable() const override { return false; }
    void show(const Notification&) override {}
};

}  // namespace

std::unique_ptr<Notifier> makeNullNotifier() { return std::make_unique<NullNotifier>(); }

#if !defined(XPCOG_HAS_GIO)
// Windows and macOS, for now. wxNotificationMessage still covers them in the wx
// build, so nothing regresses there; the GTK frontend is Linux only, so nothing
// there asks this for a notification it cannot show.
std::unique_ptr<Notifier> Notifier::create(Dispatcher) { return makeNullNotifier(); }
#endif

}  // namespace xpcog::platform
