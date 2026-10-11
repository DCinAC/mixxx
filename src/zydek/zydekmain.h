#pragma once

#include <QDeadlineTimer>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>

namespace zydek {

/// Runs fn on pContext's thread (Mixxx's main thread, for its library, effects and sound) and waits for it,
/// like Qt::BlockingQueuedConnection, but gives up when it hasn't started within timeoutMs, or once *pCancel
/// is set. A blocking call can't do that: when Mixxx shuts down, its main thread waits for the controller
/// thread (the hub's), and a hub waiting for the main thread then hung the app for good.
///
/// Returns whether fn ran. fn may capture the caller's locals by reference: once given up on, it never runs
/// (and once it has started, this waits for it to finish).
template<typename F>
bool runOnMain(QObject* pContext, F&& fn, int timeoutMs = 10000, const std::atomic<bool>* pCancel = nullptr) {
    if (!pContext) {
        return false;
    }
    if (QThread::currentThread() == pContext->thread()) {
        fn();
        return true;
    }
    struct State {
        QMutex mutex;
        QWaitCondition changed;
        enum { Pending, Running, Done, Abandoned } step = Pending;
    };
    const auto pState = std::make_shared<State>();
    QMetaObject::invokeMethod(
            pContext,
            [pState, &fn] {
                {
                    QMutexLocker lock(&pState->mutex);
                    if (pState->step == State::Abandoned) {
                        return;   // the caller has gone: fn's references are no good any more
                    }
                    pState->step = State::Running;
                }
                fn();
                QMutexLocker lock(&pState->mutex);
                pState->step = State::Done;
                pState->changed.wakeAll();
            },
            Qt::QueuedConnection);
    const QDeadlineTimer deadline(timeoutMs);
    QMutexLocker lock(&pState->mutex);
    while (pState->step != State::Done) {
        if (pState->step == State::Pending && (deadline.hasExpired() || (pCancel && pCancel->load()))) {
            pState->step = State::Abandoned;
            return false;
        }
        pState->changed.wait(&pState->mutex,
                pState->step == State::Pending ? QDeadlineTimer(100) : QDeadlineTimer(QDeadlineTimer::Forever));
    }
    return true;
}

} // namespace zydek
