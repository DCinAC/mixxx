#include "zydek/zydekpython.h"

#ifdef ZYDEK_WEB
// Python's headers use "slots" as a name, which Qt defines as a keyword.
#pragma push_macro("slots")
#undef slots
#include <Python.h>
#pragma pop_macro("slots")

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>
#include <atomic>
#include <mutex>
#include <thread>

#include "zydek/zydekjs.h"
#endif

#include <QJsonDocument>
#include <QJsonObject>

namespace zydek {
namespace python {

namespace {

QByteArray failure(const QString& message, bool starting = false) {
    QJsonObject o{{"ok", false}, {"error", message}};
    if (starting) {
        o.insert(QStringLiteral("starting"), true);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace

#ifndef ZYDEK_WEB

QByteArray call(const QString&, const QByteArray&) {
    return failure(QStringLiteral("This build of Zydek has no Web tab"));
}

#else

namespace {

enum class State { Idle, Starting, Ready, Failed };
std::atomic<State> s_state{State::Idle};
std::mutex s_errorMutex;
QString s_error;
PyObject* s_pModule = nullptr;   // zydek_web

const QString kBundle = QStringLiteral("assets:/zydek-python");   // Python + yt-dlp, from the build PC
const QString kAppCode = QStringLiteral("assets:/zydek/python");  // res/zydek/python

void fail(const QString& message) {
    qWarning() << "Zydek web:" << message;
    std::lock_guard lock(s_errorMutex);
    s_error = message;
    s_state = State::Failed;
}

QString readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

/// Copies an asset folder into dir (overwriting).
bool copyTree(const QString& from, const QString& dir) {
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    int count = 0;
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = dir + src.mid(from.size());
        QDir().mkpath(QFileInfo(dst).path());
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) {
            qWarning() << "Zydek web: can't copy" << src << "to" << dst;
            return false;
        }
        QFile::setPermissions(dst, QFile::ReadOwner | QFile::WriteOwner);
        ++count;
    }
    return count > 0;
}

// ---- _zydekjs: QuickJS for yt-dlp (see zydek_web.py) ----------------------------------------------

PyObject* jsRun(PyObject*, PyObject* args) {
    const char* code;
    Py_ssize_t len;
    Py_ssize_t stack = 8 * 1024 * 1024;
    if (!PyArg_ParseTuple(args, "s#|n", &code, &len, &stack)) {
        return nullptr;
    }
    char* error = nullptr;
    char* out = nullptr;
    Py_BEGIN_ALLOW_THREADS   // solving can take seconds: let other Python threads run meanwhile
    out = zydek_js_run(code, static_cast<size_t>(len), static_cast<size_t>(stack), &error);
    Py_END_ALLOW_THREADS
    if (!out) {
        PyErr_SetString(PyExc_RuntimeError, error ? error : "QuickJS failed");
        free(error);
        return nullptr;
    }
    PyObject* result = PyUnicode_FromString(out);
    free(out);
    return result;
}

PyObject* jsLog(PyObject*, PyObject* args) {
    const char* message;
    if (!PyArg_ParseTuple(args, "s", &message)) {
        return nullptr;
    }
    qInfo().noquote() << "Zydek web:" << QString::fromUtf8(message);
    Py_RETURN_NONE;
}

PyMethodDef kJsMethods[] = {
        {"run", jsRun, METH_VARARGS, "run(code[, stack]) -> what the script printed"},
        {"log", jsLog, METH_VARARGS, "log(message): Mixxx's log"},
        {nullptr, nullptr, 0, nullptr}};

PyModuleDef kJsModule = {PyModuleDef_HEAD_INIT, "_zydekjs", nullptr, -1, kJsMethods, nullptr, nullptr, nullptr, nullptr};

PyObject* initJsModule() {
    return PyModule_Create(&kJsModule);
}

// ---- start ----------------------------------------------------------------------------------------

void start() {
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString home = data + QStringLiteral("/zydek-python");
    const QString version = readAll(kBundle + QStringLiteral("/VERSION"));
    if (version.isEmpty()) {
        fail(QStringLiteral("Python isn't packaged in this build"));
        return;
    }
    // Python's files live in the APK; unpack them once per version (the first time takes a few seconds).
    if (readAll(home + QStringLiteral("/VERSION")) != version) {
        qInfo() << "Zydek web: unpacking Python" << version;
        QDir(home).removeRecursively();
        if (!copyTree(kBundle, home)) {
            fail(QStringLiteral("Couldn't unpack Python"));
            return;
        }
    }
    // Zydek's own Python code, fresh every start
    if (!copyTree(kAppCode, home + QStringLiteral("/app"))) {
        fail(QStringLiteral("Couldn't unpack zydek_web.py"));
        return;
    }
    QDir().mkpath(cache);
    qputenv("TMPDIR", cache.toUtf8());
    qputenv("HOME", data.toUtf8());
    qputenv("XDG_CACHE_HOME", cache.toUtf8());

    if (PyImport_AppendInittab("_zydekjs", &initJsModule) == -1) {
        fail(QStringLiteral("Couldn't register _zydekjs"));
        return;
    }
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    const auto setString = [&config](wchar_t** pField, const QString& value) {
        return PyConfig_SetString(&config, pField, reinterpret_cast<const wchar_t*>(value.toStdWString().c_str()));
    };
    const QString lib = home + QStringLiteral("/lib/python3.14");
    PyStatus status = setString(&config.home, home);
    config.module_search_paths_set = 1;
    const QStringList searchPaths{home + QStringLiteral("/app"),
            lib,
            lib + QStringLiteral("/lib-dynload"),
            lib + QStringLiteral("/site-packages")};
    for (const QString& path : searchPaths) {
        if (!PyStatus_Exception(status)) {
            status = PyWideStringList_Append(&config.module_search_paths,
                    reinterpret_cast<const wchar_t*>(path.toStdWString().c_str()));
        }
    }
    config.install_signal_handlers = 0;   // Mixxx owns the process's signals
    config.site_import = 0;
    if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
    }
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        fail(QStringLiteral("Python didn't start: %1").arg(QString::fromUtf8(status.err_msg ? status.err_msg : "?")));
        return;
    }
    s_pModule = PyImport_ImportModule("zydek_web");
    if (s_pModule) {
        PyObject* r = PyObject_CallMethod(s_pModule,
                "start",
                "sss",
                data.toUtf8().constData(),
                cache.toUtf8().constData(),
                QStandardPaths::writableLocation(QStandardPaths::MusicLocation).toUtf8().constData());
        if (!r) {
            Py_CLEAR(s_pModule);
        }
        Py_XDECREF(r);
    }
    if (!s_pModule) {
        PyErr_Print();
        PyEval_SaveThread();
        fail(QStringLiteral("zydek_web.py didn't start (see the log)"));
        return;
    }
    PyEval_SaveThread();   // let go of the GIL: calls come from other threads
    qInfo() << "Zydek web: Python" << PY_VERSION << "ready";
    s_state = State::Ready;
}

} // namespace

QByteArray call(const QString& name, const QByteArray& jsonArgs) {
    State state = s_state;
    if (state == State::Idle) {
        State idle = State::Idle;
        if (s_state.compare_exchange_strong(idle, State::Starting)) {
            std::thread(start).detach();
        }
        state = State::Starting;
    }
    if (state == State::Starting) {
        return failure(QStringLiteral("Starting Python (the first time takes a little while)…"), true);
    }
    if (state == State::Failed) {
        std::lock_guard lock(s_errorMutex);
        return failure(s_error);
    }
    const PyGILState_STATE gil = PyGILState_Ensure();
    QByteArray out;
    PyObject* r = PyObject_CallMethod(s_pModule, "api", "ss", name.toUtf8().constData(), jsonArgs.constData());
    if (r && PyUnicode_Check(r)) {
        out = QByteArray(PyUnicode_AsUTF8(r));
    } else {
        PyErr_Print();
        out = failure(QStringLiteral("Python error (see the log)"));
    }
    Py_XDECREF(r);
    PyGILState_Release(gil);
    return out;
}

#endif

} // namespace python
} // namespace zydek
