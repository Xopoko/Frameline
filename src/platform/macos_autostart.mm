#include "autostart.h"

#import <ServiceManagement/ServiceManagement.h>

namespace Autostart {
namespace {

bool isRegistered(SMAppServiceStatus status)
{
    // An unapproved registration can still be removed. macOS controls when it can run.
    return status == SMAppServiceStatusEnabled || status == SMAppServiceStatusRequiresApproval;
}

} // namespace

State snapshot(QString *error)
{
    if (error)
        error->clear();
    const auto status = SMAppService.mainAppService.status;
    if (status == SMAppServiceStatusNotFound && error)
        *error = QStringLiteral("Open the signed FreezeCut app to change its login setting.");
    if (!isRegistered(status))
        return {};
    return {true};
}

bool setEnabled(bool enabled, QString *error)
{
    if (error)
        error->clear();
    SMAppService *service = SMAppService.mainAppService;
    const auto status = service.status;
    if (status == SMAppServiceStatusNotFound) {
        if (error)
            *error = QStringLiteral("Open the signed FreezeCut app to change its login setting.");
        return false;
    }
    if (enabled == isRegistered(status))
        return true;

    NSError *registrationError = nil;
    const bool changed = enabled ? [service registerAndReturnError:&registrationError]
                                 : [service unregisterAndReturnError:&registrationError];
    if (!changed && error) {
        *error = QStringLiteral("Could not change the FreezeCut login setting.");
        if (registrationError)
            *error += u' ' + QString::fromUtf8(registrationError.localizedDescription.UTF8String);
    }
    return changed;
}

bool restore(const State &state, QString *error)
{
    return setEnabled(state.isEnabled(), error);
}

void refreshAtStartup()
{
    // Store builds keep the system-managed registration and never write a LaunchAgent.
}

} // namespace Autostart
