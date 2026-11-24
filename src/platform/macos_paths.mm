#include "macos_paths.h"

#include <QFile>

#include <array>
#include <pwd.h>
#include <unistd.h>

#import <Foundation/Foundation.h>

QString macSandboxTemporaryDirectory()
{
    return QString::fromNSString(NSTemporaryDirectory());
}

QString macAccountHomeDirectory()
{
    // Only used to recognize paths in manually copied standalone settings.
    static const QString home = [] {
        std::array<char, 16 * 1024> buffer;
        struct passwd account {};
        struct passwd *result = nullptr;
        if (getpwuid_r(geteuid(), &account, buffer.data(), buffer.size(), &result) != 0 || !result)
            return QString();
        return QFile::decodeName(account.pw_dir);
    }();
    return home;
}
