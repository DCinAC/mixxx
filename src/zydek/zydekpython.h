#pragma once

#include <QByteArray>
#include <QString>

namespace zydek {

/// Python (python.org's Android build) running the phone page's Web tab: res/zydek/python/zydek_web.py,
/// with yt-dlp for YouTube and Bilibili. Started in the background on first use; the first start unpacks
/// Python into the app's files. Only in builds made with ZYDEK_PYTHON_PREFIX (the Zydek build PC).
namespace python {

/// zydek_web.api(name, args): JSON in, JSON out, quick (downloads run in Python's own threads). While
/// Python is starting it answers {"ok": false, "starting": true}.
QByteArray call(const QString& name, const QByteArray& jsonArgs);

} // namespace python
} // namespace zydek
