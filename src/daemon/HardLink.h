#pragma once

#include <QString>

// Publishes `source` under the name `destination` as a hard link, without replacing a file already there.
// Returns false on failure; `alreadyExists` then tells a taken destination from any other error.
bool createHardLinkNoReplace(const QString &source, const QString &destination, bool *alreadyExists);
