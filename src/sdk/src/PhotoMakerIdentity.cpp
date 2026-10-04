#include "kira/service/PhotoMakerIdentity.h"
#include <archive.h>
#include <archive_entry.h>
#include <QJsonDocument>
#include <QJsonObject>
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

QByteArray KiraService::packPhotoMakerIdentity(const QByteArray &referencePng,
    const QByteArray &embeddings, QString *error)
{
    if (error) error->clear();
    if (!referencePng.startsWith(PngSignature) || embeddings.isEmpty()
        || referencePng.size() + embeddings.size() > MaxBytes) {
        fail(error, QStringLiteral("Identity requires a PNG reference and nonempty embeddings (maximum 64 MiB)."));
        return {};
    }
    const QMap<QString, QByteArray> files{
        {"manifest.json", QJsonDocument(QJsonObject{{"type", "photomaker-v2"}, {"version", 1}}).toJson(QJsonDocument::Compact)},
        {"reference.png", referencePng}, {"id_embeds.bin", embeddings}};
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    const qsizetype inputSize = referencePng.size() + embeddings.size();
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

bool KiraService::unpackPhotoMakerIdentity(const QByteArray &package, QByteArray &referencePng,
    QByteArray &embeddings, QString *error)
{
    referencePng.clear(); embeddings.clear();
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
        if ((name != "manifest.json" && name != "reference.png" && name != "id_embeds.bin")
            || files.contains(name) || archive_entry_filetype(entry) != AE_IFREG
            || archive_entry_symlink(entry) || archive_entry_hardlink(entry)
            || size <= 0 || size > MaxBytes || total + size > MaxBytes + 4096
            || (name == "manifest.json" && size > 4096))
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
    if (files.size() != 3 || manifest.value("type").toString() != "photomaker-v2"
        || manifest.value("version") != QJsonValue(1)
        || files.value("reference.png").size() + files.value("id_embeds.bin").size() > MaxBytes
        || !files.value("reference.png").startsWith(PngSignature))
        return fail(error, QStringLiteral("Unsupported or incomplete PhotoMaker identity archive."));
    referencePng = files.value("reference.png");
    embeddings = files.value("id_embeds.bin");
    return true;
}
