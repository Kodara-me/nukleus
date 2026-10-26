#pragma once
#include <QByteArray>
#include <QSize>
#include <QString>

namespace NukleusWeb
{
constexpr qint64 MaximumImageBytes = 32LL * 1024 * 1024;   // R8: one image
constexpr qint64 MaximumJobImageBytes = 96LL * 1024 * 1024; // R8: all the images of one job

struct PreparedImage
{
    QByteArray data; // the bytes to send to Nukleus
    QSize size;      // the size of the image as sent
};

// The size an image is scaled down to so that it holds at most `pixelBudget` pixels (R36): the aspect
// ratio is kept and each side is rounded down to a multiple of 64, at least 64. An image within the
// budget keeps its size.
QSize scaledSize(const QSize &size, qint64 pixelBudget);

// Decodes `data` as PNG or JPEG (R8, R46) and, when `pixelBudget` is positive and the image has more
// pixels, scales it down on its own and re-encodes it as PNG (R36); otherwise `data` is kept as it
// is. On failure, `error` names the image through `label`.
bool prepareImage(const QByteArray &data, const QString &label, qint64 pixelBudget, PreparedImage *image, QString *error);
}
