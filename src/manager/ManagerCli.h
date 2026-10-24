#pragma once

#include <QtGlobal>

class QCommandLineParser;

namespace ManagerCli
{
// Declares the options of the list, add, attach and delete commands.
void addOptions(QCommandLineParser &parser);
// Runs the command given by the parsed arguments against the local service on port.
int run(const QCommandLineParser &parser, quint16 port = 9384);
} // namespace ManagerCli
