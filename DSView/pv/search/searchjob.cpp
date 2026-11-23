/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "searchjob.h"

#include <QMetaObject>

namespace pv {
namespace search {

SearchJob::SearchJob(QObject *parent) :
    QObject(parent),
    _cancel(false),
    _progress(0),
    _total(0),
    _running(false),
    _generation(0)
{
}

SearchJob::~SearchJob()
{
    stop();
}

void SearchJob::start(const Program &program, std::unique_ptr<SampleSource> source,
                      uint64_t from, uint64_t to, uint64_t limit, Sink sink)
{
    stop();

    _cancel = false;
    _progress = from;
    _total = source->sample_count();
    _running = true;
    _result = RunResult();
    const uint64_t generation = ++_generation;

    // The thread owns its copies; nothing it uses lives in the job but the
    // atomics and the result, which done() reads after joining.
    std::shared_ptr<SampleSource> src(source.release());
    _thread = std::thread([this, program, src, from, to, limit, sink, generation](){
        _thread_result = run(program, *src, from, to, limit, _cancel, &_progress, sink);
        QMetaObject::invokeMethod(this, [this, generation](){ done(generation); },
                                  Qt::QueuedConnection);
    });
}

void SearchJob::stop()
{
    if (_thread.joinable()){
        _cancel = true;
        _thread.join();
    }
    // A done() still queued for it is ignored.
    _generation++;
    _running = false;
}

void SearchJob::done(uint64_t generation)
{
    if (generation != _generation || !_thread.joinable())
        return;
    _thread.join();
    _running = false;
    _result = _thread_result;
    emit finished();
}

} // namespace search
} // namespace pv
