#include "autostart.h"

#include <qt_windows.h>
#include <appmodel.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

#include <future>
#include <optional>
#include <system_error>

namespace Autostart {
namespace {

using winrt::Windows::ApplicationModel::StartupTask;
using winrt::Windows::ApplicationModel::StartupTaskState;

bool hasPackageIdentity()
{
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}

bool isEnabledState(StartupTaskState state)
{
    return state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy;
}

StartupTaskState startupState(std::optional<bool> requested = std::nullopt)
{
    // Store desktop apps need no consent prompt. Wait for WinRT off the UI thread's COM apartment.
    return std::async(std::launch::async, [requested] {
        winrt::init_apartment();
        struct Apartment {
            ~Apartment() { winrt::uninit_apartment(); }
        } apartment;

        const auto operation = StartupTask::GetAsync(L"FreezeCutStartup");
        const auto task = operation ? operation.get() : nullptr;
        if (!task)
            throw winrt::hresult_error(E_FAIL);
        const auto state = task.State();
        if (requested) {
            if (*requested && state == StartupTaskState::Disabled)
                return task.RequestEnableAsync().get();
            if (!*requested && state == StartupTaskState::Enabled) {
                task.Disable();
                return task.State();
            }
        }
        return state;
    }).get();
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

State snapshot(QString *error)
{
    setError(error, {});
    if (!hasPackageIdentity()) {
        setError(error, QStringLiteral("Open FreezeCut from the Microsoft Store installation to change its startup setting."));
        return {};
    }
    try {
        const auto state = startupState();
        if (state == StartupTaskState::EnabledByPolicy || state == StartupTaskState::DisabledByPolicy)
            setError(error, QStringLiteral("Windows policy controls the FreezeCut startup setting."));
        return isEnabledState(state) ? State{true} : State{};
    } catch (const winrt::hresult_error &) {
    } catch (const std::system_error &) {
    }
    setError(error, QStringLiteral("Could not read the FreezeCut startup setting."));
    return {};
}

bool setEnabled(bool enabled, QString *error)
{
    setError(error, {});
    if (!hasPackageIdentity()) {
        setError(error, QStringLiteral("Open FreezeCut from the Microsoft Store installation to change its startup setting."));
        return false;
    }
    try {
        const auto state = startupState(enabled);
        if (isEnabledState(state) == enabled)
            return true;
        if (state == StartupTaskState::DisabledByUser) {
            setError(error, QStringLiteral("Re-enable FreezeCut in Windows Settings → Apps → Startup."));
            return false;
        }
    } catch (const winrt::hresult_error &) {
    } catch (const std::system_error &) {
    }
    setError(error, QStringLiteral("Could not change the FreezeCut startup setting. Check Windows Settings → Apps → Startup."));
    return false;
}

bool restore(const State &state, QString *error)
{
    return setEnabled(state.isEnabled(), error);
}

void refreshAtStartup()
{
    // The packaged startup task and the standalone Run entry are independent.
}

} // namespace Autostart
