#include "HardLink.h"

#include <QDir>
#include <QFile>

#include <cerrno>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

bool createHardLinkNoReplace(const QString &source, const QString &destination, bool *alreadyExists)
{
    *alreadyExists = false;
#ifdef Q_OS_WIN
    // QFile::link would create a .lnk shortcut here, not a hard link.
    const QString nativeSource = QDir::toNativeSeparators(source);
    const QString nativeDestination = QDir::toNativeSeparators(destination);
    if (CreateHardLinkW(reinterpret_cast<LPCWSTR>(nativeDestination.utf16()), reinterpret_cast<LPCWSTR>(nativeSource.utf16()), nullptr)) return true;
    const DWORD error = GetLastError();
    *alreadyExists = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
    return false;
#else
    if (::link(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData()) == 0) return true;
    *alreadyExists = errno == EEXIST;
    return false;
#endif
}
