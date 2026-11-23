/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_SEARCH_SEARCHJOB_H
#define DSVIEW_PV_SEARCH_SEARCHJOB_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <QObject>

#include "searchengine.h"

namespace pv {
namespace search {

// Runs one search at a time on its own thread. The sink gets each match on
// that thread; finished() comes on the GUI thread.
class SearchJob : public QObject
{
    Q_OBJECT

public:
    using Sink = std::function<void(const Match &)>;

    explicit SearchJob(QObject *parent = nullptr);
    ~SearchJob() override;

    void start(const Program &program, std::unique_ptr<SampleSource> source,
               uint64_t from, uint64_t to, uint64_t limit, Sink sink);

    // Cancels and waits for the thread. There is no finished() for it.
    void stop();

    inline bool is_running() const { return _running; }
    inline uint64_t progress() const { return _progress.load(std::memory_order_relaxed); }
    inline uint64_t total() const { return _total; }
    // From finished() on.
    inline const RunResult &result() const { return _result; }

signals:
    void finished();

private:
    void done(uint64_t generation);

    std::thread _thread;
    std::atomic<bool> _cancel;
    std::atomic<uint64_t> _progress;
    uint64_t _total;
    bool _running;
    uint64_t _generation;
    RunResult _result;
    RunResult _thread_result;
};

} // namespace search
} // namespace pv

#endif
