// SPDX-License-Identifier: BSD-2-Clause-Patent
// Run as the desktop user. Register this executable in a local .desktop file
// with X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2 first.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QImage>
#include <QVariantMap>
#include <poll.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        qCritical() << "Usage: piano-kwin-screenshot OUTPUT.png";
        return 2;
    }
    int pipes[2];
    if (pipe(pipes)) {
        perror("pipe");
        return 1;
    }
    QDBusUnixFileDescriptor output(pipes[1]);
    close(pipes[1]);
    QDBusInterface capture("org.kde.KWin.ScreenShot2",
                           "/org/kde/KWin/ScreenShot2",
                           "org.kde.KWin.ScreenShot2",
                           QDBusConnection::sessionBus());
    capture.setTimeout(10000);
    QDBusReply<QVariantMap> reply = capture.call(
        "CaptureWorkspace", QVariantMap{{"native-resolution", true}},
        QVariant::fromValue(output));
    output = QDBusUnixFileDescriptor();
    if (!reply.isValid()) {
        qCritical() << reply.error();
        close(pipes[0]);
        return 1;
    }
    const auto meta = reply.value();
    const qint64 width = meta["width"].toUInt(), height = meta["height"].toUInt();
    const qint64 stride = meta["stride"].toUInt(), size = stride * height;
    if (meta["type"] != "raw" || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 || stride < width ||
        size <= 0 || size > 256 * 1024 * 1024) {
        qCritical() << "Invalid screenshot metadata" << meta;
        close(pipes[0]);
        return 1;
    }
    QByteArray bytes(size, Qt::Uninitialized);
    qint64 done = 0;
    while (done < size) {
        pollfd fd{pipes[0], POLLIN, 0};
        if (poll(&fd, 1, 10000) <= 0)
            break;
        const auto count = read(pipes[0], bytes.data() + done, size - done);
        if (count <= 0)
            break;
        done += count;
    }
    close(pipes[0]);
    if (done != size) {
        qCritical() << "Incomplete screenshot" << done << size;
        return 1;
    }
    QImage image(reinterpret_cast<const uchar *>(bytes.constData()), width,
                 height, stride, QImage::Format(meta["format"].toUInt()));
    if (image.isNull() || !image.save(QString::fromLocal8Bit(argv[1]), "PNG"))
        return 1;
    qInfo() << image.size() << "saved to" << argv[1];
    return 0;
}
