#include "ImageUpload.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <cmath>

namespace NukleusWeb
{
namespace
{
bool hasPngOrJpegSignature(const QByteArray &data)
{
    return data.startsWith("\x89PNG\r\n\x1a\n") || data.startsWith("\xff\xd8\xff");
}

int roundedDown(double side)
{
    return qMax(64, int(std::floor(side / 64.0)) * 64);
}
}

QSize scaledSize(const QSize &size, qint64 pixelBudget)
{
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixelBudget <= 0 || pixels <= pixelBudget) return size;
    const double factor = std::sqrt(double(pixelBudget) / double(pixels));
    return {roundedDown(size.width() * factor), roundedDown(size.height() * factor)};
}

bool prepareImage(const QByteArray &data, const QString &label, qint64 pixelBudget, PreparedImage *image, QString *error)
{
    if (!hasPngOrJpegSignature(data)) {
        *error = QStringLiteral("The image \"%1\" is not a PNG or JPEG image.").arg(label);
        return false;
    }
    QByteArray bytes = data;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    // The orientation tag is not applied: the size reported and the size sent stay those of the bytes.
    reader.setAutoTransform(false);
    const QImage decoded = reader.read();
    if (decoded.isNull()) {
        *error = QStringLiteral("The image \"%1\" cannot be decoded as PNG or JPEG.").arg(label);
        return false;
    }
    const QSize target = scaledSize(decoded.size(), pixelBudget);
    if (target == decoded.size()) {
        *image = {data, decoded.size()};
        return true;
    }
    QByteArray encoded;
    QBuffer output(&encoded);
    output.open(QIODevice::WriteOnly);
    if (!decoded.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(&output, "PNG")) {
        *error = QStringLiteral("The image \"%1\" cannot be scaled down.").arg(label);
        return false;
    }
    *image = {encoded, target};
    return true;
}
}
