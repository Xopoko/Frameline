#include "shortcuts.h"

#include <QCoreApplication>
#include <QString>

#include <array>

#if defined(Q_OS_WIN)
#include "platform/windows_shortcuts.h"
#include <QAbstractNativeEventFilter>
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#elif defined(Q_OS_LINUX)
#include "platform/portal_request.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QSocketNotifier>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <cstdlib>
#endif

namespace {

#if defined(Q_OS_MACOS)
bool systemUsesRegionShortcut()
{
    CFArrayRef shortcuts = nullptr;
    if (CopySymbolicHotKeys(&shortcuts) != noErr || !shortcuts)
        return false;

    bool enabled = false;
    for (CFIndex i = 0; i < CFArrayGetCount(shortcuts) && !enabled; ++i) {
        const auto shortcut = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(shortcuts, i));
        const auto key = static_cast<CFNumberRef>(CFDictionaryGetValue(shortcut, kHISymbolicHotKeyCode));
        const auto modifiers = static_cast<CFNumberRef>(CFDictionaryGetValue(shortcut, kHISymbolicHotKeyModifiers));
        int keyCode = 0;
        int modifierMask = 0;
        enabled = CFDictionaryGetValue(shortcut, kHISymbolicHotKeyEnabled) == kCFBooleanTrue
            && key && CFNumberGetValue(key, kCFNumberIntType, &keyCode) && keyCode == kVK_ANSI_5
            && modifiers && CFNumberGetValue(modifiers, kCFNumberIntType, &modifierMask)
            && modifierMask == (cmdKey | shiftKey);
    }
    CFRelease(shortcuts);
    return enabled;
}
#endif

#if defined(Q_OS_LINUX)
const auto shortcutsInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const auto fullId = QStringLiteral("full");
using Shortcut = QPair<QString, QVariantMap>;
using ShortcutList = QList<Shortcut>;

class PortalShortcuts : public QObject {
    Q_OBJECT

public:
    PortalShortcuts(GlobalShortcuts &owner, bool systemRegion)
        : QObject(&owner), owner_(owner), bus_(Portal::connection()), systemRegion_(systemRegion),
          regionId_(systemRegion ? QStringLiteral("region") : QStringLiteral("region-standard"))
    {
    }

    ~PortalShortcuts() override
    {
        if (!session_.isEmpty()) {
            const auto close = QDBusMessage::createMethodCall(
                Portal::service, session_, Portal::sessionInterface, QStringLiteral("Close"));
            bus_.send(close);
        }
    }

    void start()
    {
        qDBusRegisterMetaType<Shortcut>();
        qDBusRegisterMetaType<ShortcutList>();
        auto *request = new Portal::Request(this);
        connect(request, &Portal::Request::failed, this, &PortalShortcuts::unavailable);
        connect(request, &Portal::Request::responded, this,
                [this](uint code, const QVariantMap &results) {
            session_ = results.value(QStringLiteral("session_handle")).toString();
            if (code != 0 || session_.isEmpty()) {
                unavailable();
                return;
            }
            bind();
        });
        request->start(shortcutsInterface, QStringLiteral("CreateSession"), {},
                       {{QStringLiteral("session_handle_token"), Portal::token()}});
    }

    QString description(bool region) const { return descriptions_[region]; }

private slots:
    void activated(const QDBusObjectPath &session, const QString &id,
                   qulonglong, const QVariantMap &)
    {
        if (session.path() != session_)
            return;
        if (id == fullId || id == regionId_)
            emit owner_.captureRequested(id == regionId_);
    }

    void shortcutsChanged(const QDBusMessage &message)
    {
        const auto arguments = message.arguments();
        if (arguments.size() == 2 && arguments[0].value<QDBusObjectPath>().path() == session_)
            update(qdbus_cast<ShortcutList>(arguments[1]));
    }

    void closed()
    {
        session_.clear();
        unavailable();
    }

private:
    void bind()
    {
        bus_.connect(Portal::service, Portal::path, shortcutsInterface, QStringLiteral("Activated"),
                     this, SLOT(activated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        bus_.connect(Portal::service, Portal::path, shortcutsInterface, QStringLiteral("ShortcutsChanged"),
                     this, SLOT(shortcutsChanged(QDBusMessage)));
        bus_.connect(Portal::service, session_, Portal::sessionInterface, QStringLiteral("Closed"),
                     this, SLOT(closed()));

        const ShortcutList keys{
            {fullId, {{QStringLiteral("description"), QStringLiteral("Capture screen")},
                      {QStringLiteral("preferred_trigger"), QStringLiteral("CTRL+ALT+SHIFT+1")}}},
            {regionId_, {{QStringLiteral("description"), QStringLiteral("Capture region")},
                         {QStringLiteral("preferred_trigger"),
                          systemRegion_ ? QStringLiteral("LOGO+s") : QStringLiteral("CTRL+ALT+SHIFT+2")}}}
        };
        auto *request = new Portal::Request(this);
        connect(request, &Portal::Request::failed, this, &PortalShortcuts::unavailable);
        connect(request, &Portal::Request::responded, this,
                [this](uint code, const QVariantMap &results) {
            if (code == 0)
                update(qdbus_cast<ShortcutList>(results.value(QStringLiteral("shortcuts"))));
            else
                unavailable();
        });
        request->start(shortcutsInterface, QStringLiteral("BindShortcuts"),
                       {QVariant::fromValue(QDBusObjectPath(session_)), QVariant::fromValue(keys), QString{}});
    }

    void update(const ShortcutList &keys)
    {
        descriptions_ = {};
        std::array<bool, 2> registered{};
        for (const auto& [id, properties] : keys) {
            if (id == fullId || id == regionId_) {
                registered[id == regionId_] = true;
                descriptions_[id == regionId_] = properties.value(QStringLiteral("trigger_description")).toString();
            }
        }
        emit owner_.statusChanged(registered[0], registered[1],
                                  registered[0] && registered[1] ? QString{} : fallback());
    }

    void unavailable()
    {
        descriptions_ = {};
        emit owner_.statusChanged(false, false, fallback());
    }

    QString fallback() const
    {
        return systemRegion_
            ? QStringLiteral("In your desktop keyboard settings, assign Super+S to "
                 "freezecut capture region. Free the shortcut first if it is already in use.")
            : QStringLiteral("In your desktop keyboard settings, assign Ctrl+Alt+Shift+1 and Ctrl+Alt+Shift+2 "
                 "to freezecut capture full and freezecut capture region.");
    }

    GlobalShortcuts &owner_;
    QDBusConnection bus_;
    bool systemRegion_;
    QString regionId_;
    QString session_;
    std::array<QString, 2> descriptions_;
};

class X11Shortcuts {
public:
    explicit X11Shortcuts(GlobalShortcuts &owner)
        : owner_(owner), connection_(xcb_connect(nullptr, nullptr))
    {
    }

    ~X11Shortcuts()
    {
        notifier_.reset();
        if (connection_)
            xcb_disconnect(connection_);
    }

    std::array<bool, 2> start(bool systemRegion)
    {
        if (!connection_ || xcb_connection_has_error(connection_))
            return {};

        const auto *setup = xcb_get_setup(connection_);
        for (auto root = xcb_setup_roots_iterator(setup); root.rem; xcb_screen_next(&root))
            roots_.append(root.data->root);

        const auto first = setup->min_keycode;
        const auto count = setup->max_keycode - first + 1;
        const auto cookie = xcb_get_keyboard_mapping(connection_, first, count);
        const std::unique_ptr<xcb_get_keyboard_mapping_reply_t, decltype(&std::free)> map(
            xcb_get_keyboard_mapping_reply(connection_, cookie, nullptr), &std::free);
        if (!map)
            return {};
        const auto *symbols = xcb_get_keyboard_mapping_keysyms(map.get());
        auto keycode = [&](xcb_keysym_t symbol) -> xcb_keycode_t {
            for (int i = 0; i < count; ++i) {
                for (int level = 0; level < map->keysyms_per_keycode; ++level) {
                    if (symbols[i * map->keysyms_per_keycode + level] == symbol)
                        return first + i;
                }
            }
            return 0;
        };
        keycodes_ = {keycode(XK_1), keycode(systemRegion ? XK_s : XK_2)};
        const auto numLock = keycode(XK_Num_Lock);
        const auto scrollLock = keycode(XK_Scroll_Lock);
        const auto leftAlt = keycode(XK_Alt_L);
        const auto rightAlt = keycode(XK_Alt_R);
        const auto leftSuper = keycode(XK_Super_L);
        const auto rightSuper = keycode(XK_Super_R);
        const auto modCookie = xcb_get_modifier_mapping(connection_);
        const std::unique_ptr<xcb_get_modifier_mapping_reply_t, decltype(&std::free)> modifiers(
            xcb_get_modifier_mapping_reply(connection_, modCookie, nullptr), &std::free);
        uint16_t locks = XCB_MOD_MASK_LOCK;
        uint16_t alt = 0;
        uint16_t super = 0;
        if (modifiers) {
            const auto *keys = xcb_get_modifier_mapping_keycodes(modifiers.get());
            for (int i = 0; i < 8 * modifiers->keycodes_per_modifier; ++i) {
                if (!keys[i])
                    continue;
                const auto mask = 1 << (i / modifiers->keycodes_per_modifier);
                if (keys[i] == numLock || keys[i] == scrollLock)
                    locks |= mask;
                if (keys[i] == leftAlt || keys[i] == rightAlt)
                    alt |= mask;
                if (keys[i] == leftSuper || keys[i] == rightSuper)
                    super |= mask;
            }
        }
        if (!alt)
            alt = XCB_MOD_MASK_1;
        if (!super)
            super = XCB_MOD_MASK_4;
        masks_[0] = combinations(alt, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT, locks);
        masks_[1] = systemRegion ? combinations(super, 0, locks) : masks_[0];

        const std::array registered{grab(keycodes_[0], masks_[0]), grab(keycodes_[1], masks_[1])};
        notifier_ = std::make_unique<QSocketNotifier>(xcb_get_file_descriptor(connection_), QSocketNotifier::Read);
        QObject::connect(notifier_.get(), &QSocketNotifier::activated, &owner_, [this] { process(); });
        return registered;
    }

private:
    static QList<uint16_t> combinations(uint16_t modifiers, uint16_t required, uint16_t locks)
    {
        QList<uint16_t> result;
        for (uint16_t mask = locks;; mask = (mask - 1) & locks) {
            for (uint16_t bit = 1; bit <= modifiers; bit <<= 1) {
                if (modifiers & bit)
                    result.append(mask | required | bit);
            }
            if (!mask)
                return result;
        }
    }

    bool grab(xcb_keycode_t key, const QList<uint16_t> &masks)
    {
        if (!key)
            return false;
        QList<xcb_void_cookie_t> requests;
        for (const auto root : roots_) {
            for (const auto modifiers : masks) {
                requests.append(xcb_grab_key_checked(connection_, false, root, modifiers, key,
                                                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            }
        }
        bool succeeded = true;
        for (const auto request : requests) {
            if (auto *error = xcb_request_check(connection_, request)) {
                succeeded = false;
                std::free(error);
            }
        }
        if (!succeeded) {
            for (const auto root : roots_) {
                for (const auto modifiers : masks)
                    xcb_ungrab_key(connection_, key, root, modifiers);
            }
            xcb_flush(connection_);
        }
        return succeeded;
    }

    void process()
    {
        while (auto *event = xcb_poll_for_event(connection_)) {
            const auto type = event->response_type & 0x7f;
            if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
                const auto *key = reinterpret_cast<xcb_key_press_event_t *>(event);
                for (std::size_t i = 0; i < keycodes_.size(); ++i) {
                    if (keycodes_[i] != key->detail)
                        continue;
                    if (type == XCB_KEY_RELEASE)
                        releaseTime_[i] = key->time;
                    else if (releaseTime_[i] != key->time && masks_[i].contains(key->state & 0xff))
                        emit owner_.captureRequested(i == 1);
                }
            }
            std::free(event);
        }
        if (xcb_connection_has_error(connection_))
            notifier_->setEnabled(false);
    }

    GlobalShortcuts &owner_;
    xcb_connection_t *connection_;
    QList<xcb_window_t> roots_;
    std::array<QList<uint16_t>, 2> masks_;
    std::array<xcb_keycode_t, 2> keycodes_{};
    std::array<xcb_timestamp_t, 2> releaseTime_{};
    std::unique_ptr<QSocketNotifier> notifier_;
};
#endif
} // namespace

class GlobalShortcuts::Backend
#if defined(Q_OS_WIN)
    : public QAbstractNativeEventFilter
#endif
{
public:
    explicit Backend(GlobalShortcuts &owner) : owner_(owner) {}
    ~Backend()
    {
#if defined(Q_OS_WIN)
        if (started_) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
            UnregisterHotKey(nullptr, 1);
            if (!systemRegion_)
                UnregisterHotKey(nullptr, 2);
        }
#elif defined(Q_OS_MACOS)
        for (const auto key : keys_) {
            if (key)
                UnregisterEventHotKey(key);
        }
        if (handler_)
            RemoveEventHandler(handler_);
#endif
    }

    void start(RegionShortcutPreset preset)
    {
        if (started_)
            return;
        started_ = true;
        systemRegion_ = preset == RegionShortcutPreset::System;
        std::array<bool, 2> registered{};
#if defined(Q_OS_WIN)
        QCoreApplication::instance()->installNativeEventFilter(this);
        constexpr auto modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
        registered[0] = RegisterHotKey(nullptr, 1, modifiers, '1');
        if (systemRegion_) {
            regionShortcut_ = std::make_unique<WindowsRegionShortcut>([this] { emit owner_.captureRequested(true); });
            registered[1] = regionShortcut_->start();
        } else {
            registered[1] = RegisterHotKey(nullptr, 2, modifiers, '2');
        }
#elif defined(Q_OS_MACOS)
        const EventTypeSpec events[] = {{kEventClassKeyboard, kEventHotKeyPressed},
                                        {kEventClassKeyboard, kEventHotKeyReleased}};
        const auto target = GetApplicationEventTarget();
        if (InstallEventHandler(target, handleHotkey, 2, events, this, &handler_) == noErr) {
            constexpr auto modifiers = cmdKey | optionKey | shiftKey;
            registered[0] = RegisterEventHotKey(kVK_ANSI_1, modifiers, {signature, 1},
                                                target, 0, &keys_[0]) == noErr;
            if (!systemRegion_) {
                registered[1] = RegisterEventHotKey(kVK_ANSI_2, modifiers, {signature, 2},
                                                    target, 0, &keys_[1]) == noErr;
            } else if (!systemUsesRegionShortcut()) {
                registered[1] = RegisterEventHotKey(kVK_ANSI_5, cmdKey | shiftKey, {signature, 2},
                                                    target, kEventHotKeyExclusive, &keys_[1]) == noErr;
            }
        }
#elif defined(Q_OS_LINUX)
        if (Portal::isWayland()) {
            portal_ = std::make_unique<PortalShortcuts>(owner_, systemRegion_);
            portal_->start();
            return;
        }
        x11_ = std::make_unique<X11Shortcuts>(owner_);
        registered = x11_->start(systemRegion_);
#endif
        QString warning;
#if defined(Q_OS_MACOS)
        if (systemRegion_ && !registered[1]) {
            warning = QStringLiteral("⌘⇧5 is unavailable. Disable Screenshot and recording options in System Settings → "
                         "Keyboard → Keyboard Shortcuts → Screenshots, then restart FreezeCut.");
        }
#endif
        if (warning.isEmpty() && (!registered[0] || !registered[1])) {
            warning = QStringLiteral("Some shortcuts could not be registered. You can still capture from the FreezeCut window.");
        }
        emit owner_.statusChanged(registered[0], registered[1], warning);
    }

    QString description(bool region) const
    {
#if defined(Q_OS_LINUX)
        if (portal_) {
            const auto description = portal_->description(region);
            if (!description.isEmpty())
                return description;
        }
#endif
        return defaultKeys(region, systemRegion_ ? RegionShortcutPreset::System : RegionShortcutPreset::Standard);
    }

private:
#if defined(Q_OS_WIN)
    std::unique_ptr<WindowsRegionShortcut> regionShortcut_;

    bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override
    {
        const auto *event = static_cast<MSG *>(message);
        if (event->message != WM_HOTKEY
            || (event->wParam != 1 && (event->wParam != 2 || systemRegion_)))
            return false;
        if (result)
            *result = 0;
        emit owner_.captureRequested(event->wParam == 2);
        return true;
    }
#elif defined(Q_OS_MACOS)
    static OSStatus handleHotkey(EventHandlerCallRef, EventRef event, void *context)
    {
        EventHotKeyID key{};
        if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                              nullptr, sizeof(key), nullptr, &key) != noErr
            || key.signature != signature || key.id < 1 || key.id > 2)
            return eventNotHandledErr;

        auto &self = *static_cast<Backend *>(context);
        auto &held = self.held_[key.id - 1];
        if (GetEventKind(event) == kEventHotKeyReleased) {
            held = false;
        } else if (!held) {
            held = true;
            emit self.owner_.captureRequested(key.id == 2);
        }
        return noErr;
    }

    static constexpr OSType signature = 0x5346524d;
    EventHandlerRef handler_ = nullptr;
    std::array<EventHotKeyRef, 2> keys_{};
    std::array<bool, 2> held_{};
#elif defined(Q_OS_LINUX)
    std::unique_ptr<PortalShortcuts> portal_;
    std::unique_ptr<X11Shortcuts> x11_;
#endif
    GlobalShortcuts &owner_;
    bool started_ = false;
    bool systemRegion_ = false;
};

GlobalShortcuts::GlobalShortcuts(QObject *parent)
    : QObject(parent), backend_(std::make_unique<Backend>(*this))
{
}

GlobalShortcuts::~GlobalShortcuts() = default;

void GlobalShortcuts::start(RegionShortcutPreset preset)
{
    backend_->start(preset);
}

QString GlobalShortcuts::description(bool region) const
{
    return backend_->description(region);
}

QString GlobalShortcuts::defaultKeys(bool region, RegionShortcutPreset preset)
{
    if (region && preset == RegionShortcutPreset::System) {
#if defined(Q_OS_MACOS)
        return QStringLiteral("⌘⇧5");
#elif defined(Q_OS_WIN)
        return QStringLiteral("Win+S");
#else
        return QStringLiteral("Super+S");
#endif
    }
#if defined(Q_OS_MACOS)
    return region ? QStringLiteral("⌘⌥⇧2") : QStringLiteral("⌘⌥⇧1");
#else
    return region ? QStringLiteral("Ctrl+Alt+Shift+2") : QStringLiteral("Ctrl+Alt+Shift+1");
#endif
}

#if defined(Q_OS_LINUX)
#include "shortcuts.moc"
#endif
