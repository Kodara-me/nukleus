#include "ManageModelsWindow.h"

#include "ManagerServiceClient.h"

#include <QApplication>
#include <KiraUi/Theme.h>
#include <QIcon>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTextStream>

#include <memory>
#include <unistd.h>

namespace
{
QString sizeText(qint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 bytes").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

bool hasArgument(int argc, char *argv[], const QString &argument)
{
    for (int index = 1; index < argc; ++index) if (QString::fromLocal8Bit(argv[index]) == argument) return true;
    return false;
}

int runCli(QCoreApplication &application, const QCommandLineParser &parser)
{
    Q_UNUSED(application)
    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        QTextStream(stderr) << "A command is required: list, add, or delete. Use --help for usage.\n";
        return 2;
    }
    const QString command = positional.first();
    if (command != QStringLiteral("list") && command != QStringLiteral("add") && command != QStringLiteral("delete")) {
        QTextStream(stderr) << "Unknown command: " << command << ". Use --help for usage.\n";
        return 2;
    }
    if (command == QStringLiteral("list") && positional.size() != 1) {
        QTextStream(stderr) << "list does not accept an argument.\n";
        return 2;
    }
    if (command == QStringLiteral("add") && positional.size() != 2) {
        QTextStream(stderr) << "add requires <url> and --category <category>.\n";
        return 2;
    }
    if (command == QStringLiteral("add") && parser.value(QStringLiteral("category")).isEmpty()) {
        QTextStream(stderr) << "add requires --category <category>.\n";
        return 2;
    }
    const QString category = parser.value(QStringLiteral("category"));
    int companionCount = 0;
    for (const QString &option : {QStringLiteral("companion-lora"), QStringLiteral("mmproj"), QStringLiteral("vae")}) {
        if (!parser.isSet(option)) continue;
        ++companionCount;
        const bool validCategory = option == QStringLiteral("companion-lora") ? category == QStringLiteral("ip-adapter")
            : option == QStringLiteral("mmproj") ? category == QStringLiteral("llm-chat")
            : category == QStringLiteral("edition") || category == QStringLiteral("image-generation");
        if (!validCategory || companionCount > 1) {
            QTextStream(stderr) << "Use one optional companion URL matching the model category.\n";
            return 2;
        }
    }
    if (command == QStringLiteral("delete") && positional.size() != 2) {
        QTextStream(stderr) << "delete requires <id>.\n";
        return 2;
    }
    const QString argument = positional.value(1);
    if (command == QStringLiteral("delete") && !parser.isSet(QStringLiteral("yes"))) {
        if (!isatty(STDIN_FILENO)) {
            QTextStream(stderr) << "delete requires --yes when standard input is not interactive.\n";
            return 2;
        }
        QTextStream prompt(stdout);
        prompt << "Delete " << argument << "? This cannot be undone. [y/N] " << Qt::flush;
        const QString answer = QTextStream(stdin).readLine().trimmed();
        if (answer.compare(QStringLiteral("y"), Qt::CaseInsensitive) != 0 && answer.compare(QStringLiteral("yes"), Qt::CaseInsensitive) != 0) return 0;
    }

    ManagerServiceClient client;
    client.setAutomaticReconnect(false);
    QEventLoop loop;
    QTimer connectionTimeout;
    connectionTimeout.setSingleShot(true);
    connectionTimeout.setInterval(10000);
    bool finished = false;
    int exitCode = 1;
    auto finish = [&](int code, const QString &message = QString()) {
        if (finished) return;
        finished = true;
        exitCode = code;
        if (!message.isEmpty()) QTextStream(stderr) << message << '\n';
        loop.quit();
    };
    QObject::connect(&connectionTimeout, &QTimer::timeout, &loop, [&] { finish(1, QStringLiteral("The local Kira Service is unavailable.")); });
    QObject::connect(&client, &ManagerServiceClient::serviceStateChanged, &loop, [&](const QString &state, const QString &message) {
        if (state == QStringLiteral("connected")) connectionTimeout.stop();
        if (state == QStringLiteral("connection-error") || state == QStringLiteral("authentication-failed")) finish(1, message);
    });

    if (command == QStringLiteral("list")) {
        QObject::connect(&client, &ManagerServiceClient::modelsChanged, &loop, [&] {
            if (!client.isAuthenticated() || !client.modelListAvailable()) return;
            struct ModelRow
            {
                QString category;
                QString id;
                QString fileName;
                QString companion;
                QString size;
            };
            QList<ModelRow> rows;
            for (const QString &category : {QStringLiteral("image-generation"), QStringLiteral("edition"), QStringLiteral("upscaler"), QStringLiteral("llm-chat"), QStringLiteral("ip-adapter")}) {
                const QList<ManagedModelInfo> models = client.models(category);
                if (models.isEmpty()) {
                    rows.append({category, QStringLiteral("(none)"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-")});
                    continue;
                }
                for (const ManagedModelInfo &model : models) rows.append({category, model.id, model.fileName, model.companionFileName.isEmpty() ? QStringLiteral("-") : model.companionFileName, sizeText(model.fileSize)});
            }

            int categoryWidth = QStringLiteral("Category").size();
            int idWidth = QStringLiteral("ID").size();
            int fileNameWidth = QStringLiteral("Filename").size();
            int companionWidth = QStringLiteral("Companion").size();
            int sizeWidth = QStringLiteral("Size").size();
            for (const ModelRow &row : rows) {
                categoryWidth = qMax(categoryWidth, row.category.size());
                idWidth = qMax(idWidth, row.id.size());
                fileNameWidth = qMax(fileNameWidth, row.fileName.size());
                companionWidth = qMax(companionWidth, row.companion.size());
                sizeWidth = qMax(sizeWidth, row.size.size());
            }

            QTextStream output(stdout);
            output << QStringLiteral("Category").leftJustified(categoryWidth) << "  "
                   << QStringLiteral("ID").leftJustified(idWidth) << "  "
                   << QStringLiteral("Filename").leftJustified(fileNameWidth) << "  "
                   << QStringLiteral("Companion").leftJustified(companionWidth) << "  "
                   << QStringLiteral("Size").rightJustified(sizeWidth) << '\n';
            output << QString(categoryWidth, '-') << "  "
                   << QString(idWidth, '-') << "  "
                   << QString(fileNameWidth, '-') << "  "
                   << QString(companionWidth, '-') << "  "
                   << QString(sizeWidth, '-') << '\n';
            for (const ModelRow &row : rows) {
                output << row.category.leftJustified(categoryWidth) << "  "
                       << row.id.leftJustified(idWidth) << "  "
                       << row.fileName.leftJustified(fileNameWidth) << "  "
                       << row.companion.leftJustified(companionWidth) << "  "
                       << row.size.rightJustified(sizeWidth) << '\n';
            }
            finish(0);
        });
    } else if (command == QStringLiteral("add")) {
        QString installId;
        QObject::connect(&client, &ManagerServiceClient::serviceStateChanged, &loop, [&](const QString &state, const QString &) {
            if (state != QStringLiteral("connected") || !installId.isEmpty()) return;
            installId = client.installModel(parser.value(QStringLiteral("category")), QUrl(argument), QUrl(parser.isSet(QStringLiteral("mmproj")) ? parser.value(QStringLiteral("mmproj")) : parser.isSet(QStringLiteral("vae")) ? parser.value(QStringLiteral("vae")) : parser.value(QStringLiteral("companion-lora"))));
            if (installId.isEmpty()) finish(1, QStringLiteral("Could not start model installation."));
        });
        QObject::connect(&client, &ManagerServiceClient::installsChanged, &loop, [&] {
            if (installId.isEmpty()) return;
            for (const ManagedInstall &install : client.installs()) {
                if (install.installId != installId) continue;
                if (install.state == ManagedInstallState::Downloading && install.totalBytes.has_value()) QTextStream(stdout) << "Downloading " << install.bytesDownloaded << " / " << *install.totalBytes << " bytes\r" << Qt::flush;
                if (install.state == ManagedInstallState::Completed) { QTextStream(stdout) << "\nInstalled " << install.url.fileName() << '\n'; finish(0); }
                if (install.state == ManagedInstallState::Canceled) finish(1, QStringLiteral("Model installation canceled."));
                if (install.state == ManagedInstallState::Failed) finish(1, install.message);
            }
        });
    } else {
        const QString id = argument;
        QObject::connect(&client, &ManagerServiceClient::serviceStateChanged, &loop, [&](const QString &state, const QString &) {
            if (state == QStringLiteral("connected") && !client.deleteModel(id)) finish(1, QStringLiteral("Could not delete the model."));
        });
        QObject::connect(&client, &ManagerServiceClient::modelDeleted, &loop, [&](const QString &deletedId) { if (deletedId == id) finish(0); });
        QObject::connect(&client, &ManagerServiceClient::modelOperationFailed, &loop, [&](const QString &failedId, const QString &message) { if (failedId == id) finish(1, message); });
    }

    connectionTimeout.start();
    client.connectToService();
    if (!finished) loop.exec();
    return exitCode;
}
}

int main(int argc, char *argv[])
{
    const bool guiRequested = hasArgument(argc, argv, QStringLiteral("--gui"));
    std::unique_ptr<QCoreApplication> application;
    if (guiRequested) application = std::make_unique<QApplication>(argc, argv);
    else application = std::make_unique<QCoreApplication>(argc, argv);
    application->setOrganizationName(QStringLiteral("Kira Service"));
    application->setApplicationName(QStringLiteral("Kira Service Manager"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Manage models installed by the local Kira Service."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("gui"), QStringLiteral("Start the graphical manager.")});
    parser.addOption({QStringLiteral("category"), QStringLiteral("Model category for add."), QStringLiteral("category")});
    parser.addOption({QStringLiteral("companion-lora"), QStringLiteral("Optional companion LoRA URL for an IP-Adapter add."), QStringLiteral("url")});
    parser.addOption({QStringLiteral("mmproj"), QStringLiteral("Optional mmproj URL for an LLM add."), QStringLiteral("url")});
    parser.addOption({QStringLiteral("vae"), QStringLiteral("Optional VAE URL for an Edition or Image-generation add."), QStringLiteral("url")});
    parser.addOption({QStringLiteral("yes"), QStringLiteral("Confirm deletion without prompting.")});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("list, add, or delete."));
    parser.addPositionalArgument(QStringLiteral("argument"), QStringLiteral("URL for add or opaque ID for delete."), QStringLiteral("[url-or-id]"));
    parser.process(*application);

    if (guiRequested) {
        if (!parser.positionalArguments().isEmpty() || parser.isSet(QStringLiteral("category")) || parser.isSet(QStringLiteral("companion-lora")) || parser.isSet(QStringLiteral("mmproj")) || parser.isSet(QStringLiteral("vae")) || parser.isSet(QStringLiteral("yes"))) {
            QTextStream(stderr) << "--gui cannot be combined with a CLI command.\n";
            return 2;
        }
        KiraUi::Theme::apply(*static_cast<QApplication *>(application.get()));
        QIcon icon;
        for (const int size : {16, 32, 48, 64, 128, 256, 512, 1024}) icon.addFile(QStringLiteral(":/manager/icons/manager-%1.png").arg(size));
        QApplication::setWindowIcon(icon);
        auto *window = new ManageModelsWindow;
        window->show();
        return application->exec();
    }
    if (parser.positionalArguments().isEmpty()) parser.showHelp(2);
    return runCli(*application, parser);
}
