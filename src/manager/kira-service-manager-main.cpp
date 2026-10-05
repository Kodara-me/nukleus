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
        QTextStream(stderr) << "A command is required: list, add, attach, or delete. Use --help for usage.\n";
        return 2;
    }
    const QString command = positional.first();
    if (command != QStringLiteral("list") && command != QStringLiteral("add") && command != QStringLiteral("attach") && command != QStringLiteral("delete")) {
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
    const QString category = command == "attach" ? positional.value(1).section(':', 0, 0) : parser.value("category");
    if (command == "attach" && (positional.size() != 2 || (category != "image-generation" && category != "edition" && category != "ip_adapter" && category != "ip-adapter"))) {
        QTextStream(stderr) << "attach requires an opaque image-generation, edition or IP-Adapter model ID.\n"; return 2;
    }
    if (parser.value("model-type") != "checkpoint" && parser.value("model-type") != "diffusion") {
        QTextStream(stderr) << "--model-type must be checkpoint or diffusion.\n"; return 2;
    }
    if ((parser.isSet("clip-l") || parser.isSet("clip-g") || parser.isSet("text-encoder") || parser.isSet("model-type")) &&
        ((command != "add" && command != "attach") || (category != "image-generation" && category != "edition"))) {
        QTextStream(stderr) << "CLIP files and model type require an image model add or attach command.\n"; return 2;
    }
    int companionCount = 0;
    for (const QString &option : {QStringLiteral("companion-lora"), QStringLiteral("mmproj"), QStringLiteral("vae")}) {
        if (!parser.isSet(option)) continue;
        if (option == "mmproj" && (category == "image-generation" || category == "edition")
            && (command == "add" || command == "attach")) continue;
        ++companionCount;
        const bool validCategory = option == QStringLiteral("companion-lora") ? (category == QStringLiteral("ip_adapter") || category == QStringLiteral("ip-adapter"))
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
    QString installId;
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
            for (const QString &category : {QStringLiteral("image-generation"), QStringLiteral("edition"), QStringLiteral("upscaler"), QStringLiteral("llm-chat"), QStringLiteral("ip_adapter"), QStringLiteral("clip_vision"), QStringLiteral("photomaker"), QStringLiteral("prompt-enhancer")}) {
                const QList<ManagedModelInfo> models = client.models(category);
                if (models.isEmpty()) {
                    rows.append({category, QStringLiteral("(none)"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-")});
                    continue;
                }
                for (const auto &model : models) {
                    auto files = model.companionFiles;
                    if (!model.companionFileName.isEmpty()) files.insert(model.companionRole, model.companionFileName);
                    QStringList names;
                    for (auto it = files.begin(); it != files.end(); ++it) names.append(it.key() + ":" + it.value().toString());
                    rows.append({category, model.id, model.displayLabel(), names.isEmpty() ? QString("-") : names.join(", "), sizeText(model.fileSize)});
                }
            }

            int categoryWidth = QStringLiteral("Category").size();
            int idWidth = QStringLiteral("ID").size();
            int fileNameWidth = QStringLiteral("Name").size();
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
                   << QStringLiteral("Name").leftJustified(fileNameWidth) << "  "
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
    } else if (command == QStringLiteral("add") || command == "attach") {
        QObject::connect(&client, &ManagerServiceClient::serviceStateChanged, &loop, [&](const QString &state, const QString &) {
            if (state != QStringLiteral("connected") || !installId.isEmpty()) return;
            QJsonObject files;
            if (parser.isSet("clip-l")) files.insert("clip-l", parser.value("clip-l"));
            if (parser.isSet("clip-g")) files.insert("clip-g", parser.value("clip-g"));
            if (parser.isSet("text-encoder")) files.insert("llm", parser.value("text-encoder"));
            const bool image = category == "image-generation" || category == "edition";
            if (image && parser.isSet("mmproj")) files.insert("mmproj", parser.value("mmproj"));
            const auto companion = QUrl(!image && parser.isSet("mmproj") ? parser.value("mmproj") : parser.isSet("vae") ? parser.value("vae") : parser.value("companion-lora"));
            installId = client.installModel(category, command == "attach" ? QUrl{} : QUrl(argument),
                companion, {}, files, command == "attach" && !parser.isSet("model-type") ? QString() : parser.value("model-type"),
                command == "attach" ? argument : QString(), {},
                parser.isSet("display-name") ? std::optional<QString>(parser.value("display-name")) : std::nullopt);
            if (installId.isEmpty()) finish(1, QStringLiteral("Could not start model installation."));
        });
        QObject::connect(&client, &ManagerServiceClient::installsChanged, &loop, [&] {
            if (installId.isEmpty()) return;
            for (const ManagedInstall &install : client.installs()) {
                if (install.installId != installId) continue;
                if (install.state == ManagedInstallState::Downloading && install.totalBytes.has_value()) QTextStream(stdout) << "Downloading " << install.bytesDownloaded << " / " << *install.totalBytes << " bytes\r" << Qt::flush;
                if (install.state == ManagedInstallState::Completed) {
                    QTextStream(stdout) << (command == "attach" ? "\nUpdated " : "\nInstalled ")
                        << (command == "attach" ? argument : install.url.fileName()) << '\n';
                    finish(0);
                }
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
    parser.addOption({"display-name", "Optional model display name for add/attach.", "name"});
    parser.addOption({QStringLiteral("companion-lora"), QStringLiteral("Optional auxiliary LoRA URL for an IP-Adapter add/attach."), QStringLiteral("url")});
    parser.addOption({QStringLiteral("mmproj"), QStringLiteral("Optional mmproj URL for LLM add or image add/attach."), QStringLiteral("url")});
    parser.addOption({QStringLiteral("vae"), QStringLiteral("Optional VAE URL for an Edition or Image-generation add/attach."), QStringLiteral("url")});
    parser.addOption({"clip-l", "Optional CLIP-L URL for image add/attach.", "url"});
    parser.addOption({"text-encoder", "Optional LLM text encoder URL for image add/attach.", "url"});
    parser.addOption({"clip-g", "Optional CLIP-G URL for image add/attach.", "url"});
    parser.addOption({"model-type", "Model contents: checkpoint or diffusion (standalone UNet).", "type", "checkpoint"});
    parser.addOption({QStringLiteral("yes"), QStringLiteral("Confirm deletion without prompting.")});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("list, add, attach, or delete."));
    parser.addPositionalArgument(QStringLiteral("argument"), QStringLiteral("URL for add or opaque ID for attach/delete."), QStringLiteral("[url-or-id]"));
    parser.process(*application);

    if (guiRequested) {
        if (parser.isSet("display-name")) { QTextStream(stderr) << "--gui cannot be combined with --display-name.\n"; return 2; }
        if (!parser.positionalArguments().isEmpty() || parser.isSet(QStringLiteral("category")) || parser.isSet(QStringLiteral("companion-lora")) || parser.isSet(QStringLiteral("mmproj")) || parser.isSet(QStringLiteral("vae")) || parser.isSet(QStringLiteral("yes")) || parser.isSet("clip-l") || parser.isSet("clip-g") || parser.isSet("text-encoder") || parser.isSet("model-type")) {
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
