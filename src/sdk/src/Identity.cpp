#include "kira/service/Identity.h"
#include <archive.h>
#include <archive_entry.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <memory>

namespace {
constexpr qsizetype MaxBytes = 64 * 1024 * 1024;
const QByteArray PngSignature = QByteArray::fromHex("89504e470d0a1a0a");
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
QString archiveError(archive *handle)
{
    const char *message = archive_error_string(handle);
    return message ? QString::fromUtf8(message) : QStringLiteral("Invalid identity archive.");
}
}

QByteArray KiraService::packIdentity(const IdentityPackage& identity, QString* error)
{
    if (error) error->clear();
    qsizetype inputSize = 0;
    bool valid = !identity.references.isEmpty() && identity.references.size() <= 4;
    for (const auto& image : identity.references) {
        valid = valid && image.startsWith(PngSignature);
        inputSize += image.size();
    }
    if (!valid || inputSize > MaxBytes
        || identity.seed < -1 || identity.identityPrompt.size() + identity.hairPrompt.size()
            + identity.generationPrompt.size() + identity.editPrompt.size() > 65536) {
        fail(error, QStringLiteral("Identity requires one to four PNG references (maximum 64 MiB)."));
        return {};
    }
    QMap<QString, QByteArray> files;
    QJsonArray references;
    for (int i = 0; i < identity.references.size(); ++i) {
        const auto name = i == 0 ? QStringLiteral("reference.png") : QStringLiteral("reference-%1.png").arg(i + 1);
        files.insert(name, identity.references[i]);
        references.append(name);
    }
    const auto manifest = QJsonDocument(QJsonObject{{"type", "kira-identity"}, {"version", 2},
        {"references", references}, {"identityPrompt", identity.identityPrompt},
        {"hairPrompt", identity.hairPrompt}, {"generationPrompt", identity.generationPrompt},
        {"editPrompt", identity.editPrompt}, {"seed", identity.seed}}).toJson(QJsonDocument::Compact);
    if (manifest.size() > 256 * 1024) { fail(error, "Identity metadata is too large."); return {}; }
    files.insert("manifest.json", manifest);
    inputSize += manifest.size();
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    QByteArray output(inputSize + inputSize / 100 + 65536, Qt::Uninitialized);
    size_t written = 0;
    if (archive_write_set_format_zip(writer.get()) != ARCHIVE_OK
        || archive_write_set_options(writer.get(), "zip:compression=deflate") != ARCHIVE_OK
        || archive_write_open_memory(writer.get(), output.data(), output.size(), &written) != ARCHIVE_OK) {
        fail(error, archiveError(writer.get())); return {};
    }
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        const QByteArray name = it.key().toUtf8();
        archive_entry_set_pathname(entry.get(), name.constData());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0600);
        archive_entry_set_size(entry.get(), it.value().size());
        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK
            || archive_write_data(writer.get(), it.value().constData(), it.value().size()) != it.value().size()) {
            fail(error, archiveError(writer.get())); return {};
        }
    }
    if (archive_write_close(writer.get()) != ARCHIVE_OK) {
        fail(error, archiveError(writer.get())); return {};
    }
    output.resize(static_cast<qsizetype>(written));
    return output;
}

bool KiraService::unpackIdentity(const QByteArray& package, IdentityPackage& identity, QString* error)
{
    identity = {};
    if (error) error->clear();
    if (package.isEmpty() || package.size() > MaxBytes + 1024 * 1024)
        return fail(error, QStringLiteral("Identity archive is empty or exceeds 65 MiB."));
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    archive_read_support_format_zip_seekable(reader.get());
    if (archive_read_open_memory(reader.get(), package.constData(), package.size()) != ARCHIVE_OK)
        return fail(error, archiveError(reader.get()));
    QMap<QString, QByteArray> files;
    qsizetype total = 0;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        const auto size = archive_entry_size(entry);
        if ((name != "manifest.json" && name != "reference.png"
                && name != "reference-2.png" && name != "reference-3.png" && name != "reference-4.png")
            || files.contains(name) || archive_entry_filetype(entry) != AE_IFREG
            || archive_entry_symlink(entry) || archive_entry_hardlink(entry)
            || size <= 0 || size > MaxBytes || total + size > MaxBytes + 256 * 1024
            || (name == "manifest.json" && size > 256 * 1024))
            return fail(error, QStringLiteral("Identity archive contains invalid, duplicate or oversized entries."));
        QByteArray bytes;
        char buffer[16384];
        la_ssize_t count = 0;
        while ((count = archive_read_data(reader.get(), buffer, sizeof(buffer))) > 0) {
            if (bytes.size() + count > size)
                return fail(error, QStringLiteral("Identity archive entry exceeds its declared size."));
            bytes.append(buffer, count);
        }
        if (count < 0 || bytes.size() != size) return fail(error, archiveError(reader.get()));
        total += bytes.size();
        files.insert(name, bytes);
    }
    if (status != ARCHIVE_EOF) return fail(error, archiveError(reader.get()));
    const auto manifest = QJsonDocument::fromJson(files.value("manifest.json")).object();
    if (manifest.value("type").toString() != "kira-identity"
        || manifest.value("version") != QJsonValue(2)
        || files.value("reference.png").size() > MaxBytes
        || !files.value("reference.png").startsWith(PngSignature))
        return fail(error, QStringLiteral("Unsupported or incomplete identity archive."));
    QJsonArray references = manifest.value("references").toArray();
    if (!manifest.contains("references")) references = QJsonArray{QStringLiteral("reference.png")};
    if (references.isEmpty() || references.size() > 4 || files.size() != references.size() + 1
        || (manifest.contains("identityPrompt") && !manifest.value("identityPrompt").isString())
        || (manifest.contains("hairPrompt") && !manifest.value("hairPrompt").isString())
        || (manifest.contains("generationPrompt") && !manifest.value("generationPrompt").isString())
        || (manifest.contains("editPrompt") && !manifest.value("editPrompt").isString()))
        return fail(error, "Invalid identity metadata.");
    IdentityPackage decoded;
    for (int i = 0; i < references.size(); ++i) {
        const auto name = i == 0 ? QStringLiteral("reference.png") : QStringLiteral("reference-%1.png").arg(i + 1);
        if (references[i] != QJsonValue(name) || !files.value(name).startsWith(PngSignature))
            return fail(error, "Invalid identity reference list.");
        decoded.references.append(files.value(name));
    }
    qsizetype payload = 0;
    for (const auto& image : decoded.references) payload += image.size();
    if (payload > MaxBytes) return fail(error, "Invalid identity payload.");
    if (manifest.contains("seed") && (!manifest.value("seed").isDouble()
        || manifest.value("seed").toDouble() != manifest.value("seed").toInt(-2)
        || manifest.value("seed").toInt(-2) < -1)) return fail(error, "Invalid identity seed.");
    decoded.seed = manifest.value("seed").toInt(-1);
    decoded.identityPrompt = manifest.value("identityPrompt").toString();
    decoded.hairPrompt = manifest.value("hairPrompt").toString();
    decoded.generationPrompt = manifest.value("generationPrompt").toString();
    decoded.editPrompt = manifest.value("editPrompt").toString();
    identity = decoded;
    return true;
}
