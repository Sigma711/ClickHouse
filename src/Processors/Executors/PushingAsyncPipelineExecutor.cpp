#include <Processors/Executors/PushingAsyncPipelineExecutor.h>
#include <Processors/Executors/PipelineExecutor.h>
#include <Processors/ISource.h>
#include <QueryPipeline/QueryPipeline.h>
#include <QueryPipeline/ReadProgressCallback.h>
#include <Common/ThreadPool.h>
#include <Common/setThreadName.h>
#include <Common/scope_guard_safe.h>
#include <Common/CurrentThread.h>
#include <Poco/Event.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int QUERY_WAS_CANCELLED;
}

class PushingAsyncSource : public ISource
{
public:
    explicit PushingAsyncSource(SharedHeader header)
        : ISource(header)
    {}

    String getName() const override { return "PushingAsyncSource"; }

    bool setData(Chunk chunk)
    {
        std::unique_lock lock(mutex);
        condvar.wait(lock, [this] { return !has_data || is_finished; });

        if (is_finished)
            return false;

        data.swap(chunk);
        has_data = true;
        condvar.notify_one();

        return true;
    }

    void finish()
    {
        std::unique_lock lock(mutex);
        is_finished = true;
        condvar.notify_all();
    }

protected:

    Chunk generate() override
    {
        std::unique_lock lock(mutex);
        condvar.wait(lock, [this] { return has_data || is_finished; });

        Chunk res;

        res.swap(data);
        has_data = false;
        condvar.notify_one();

        return res;
    }

private:
    Chunk data;
    bool has_data = false;
    bool is_finished = false;
    std::mutex mutex;
    std::condition_variable condvar;
};

struct PushingAsyncPipelineExecutor::Data
{
    PipelineExecutorPtr executor;
    std::exception_ptr exception;
    PushingAsyncSource * source = nullptr;
    std::atomic_bool is_finished = false;
    std::atomic_bool has_exception = false;
    ThreadFromGlobalPool thread;
    Poco::Event finish_event;

    ~Data()
    {
        if (thread.joinable())
            thread.join();
    }

    void rethrowExceptionIfHas()
    {
        if (has_exception.exchange(false))
            std::rethrow_exception(exception);
    }
};

static void threadFunction(
    PushingAsyncPipelineExecutor::Data & data, ThreadGroupPtr thread_group, size_t num_threads, bool concurrency_control)
{
    try
    {
        ThreadGroupSwitcher switcher(thread_group, "QueryPushPipeEx");

        data.executor->execute(num_threads, concurrency_control);
    }
    catch (...)
    {
        data.exception = std::current_exception();
        data.has_exception = true;
    }

    if (data.source)
        data.source->finish();

    data.is_finished = true;
    data.finish_event.set();
}


PushingAsyncPipelineExecutor::PushingAsyncPipelineExecutor(QueryPipeline & pipeline_) : pipeline(pipeline_)
{
    if (!pipeline.pushing())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Pipeline for PushingPipelineExecutor must be pushing");

    pushing_source = std::make_shared<PushingAsyncSource>(pipeline.input->getSharedHeader());
    connect(pushing_source->getPort(), *pipeline.input);
    pipeline.processors->emplace_back(pushing_source);
}

PushingAsyncPipelineExecutor::~PushingAsyncPipelineExecutor()
{
    /// It must be finalized explicitly. Otherwise we cancel it assuming it's due to an exception.
    chassert(finished || std::uncaught_exceptions() || std::current_exception());
    try
    {
        cancel();
    }
    catch (...)
    {
        tryLogCurrentException("PushingAsyncPipelineExecutor");
    }
}

const Block & PushingAsyncPipelineExecutor::getHeader() const
{
    return pushing_source->getPort().getHeader();
}


void PushingAsyncPipelineExecutor::start()
{
    if (started)
        return;

    started = true;

    data = std::make_unique<Data>();
    data->executor = std::make_shared<PipelineExecutor>(pipeline.processors, pipeline.process_list_element);
    data->executor->setReadProgressCallback(pipeline.getReadProgressCallback());
    data->source = pushing_source.get();

    auto func = [&, thread_group = CurrentThread::getGroup()]()
    {
        threadFunction(*data, thread_group, pipeline.getNumThreads(), pipeline.getConcurrencyControl());
    };

    data->thread = ThreadFromGlobalPool(std::move(func));
}

[[noreturn]] static void throwOnExecutionStatus(PipelineExecutor::ExecutionStatus status)
{
    if (status == PipelineExecutor::ExecutionStatus::CancelledByTimeout
        || status == PipelineExecutor::ExecutionStatus::CancelledByUser)
        throw Exception(ErrorCodes::QUERY_WAS_CANCELLED, "Query was cancelled");

    throw Exception(ErrorCodes::LOGICAL_ERROR,
        "Pipeline for PushingPipelineExecutor was finished before all data was inserted");
}

void PushingAsyncPipelineExecutor::push(Chunk chunk)
{
    if (!started)
        start();

    bool is_pushed = pushing_source->setData(std::move(chunk));
    data->rethrowExceptionIfHas();

    if (!is_pushed)
        throwOnExecutionStatus(data->executor->getExecutionStatus());
}

void PushingAsyncPipelineExecutor::push(Block block)
{
    push(Chunk(block.getColumns(), block.rows()));
}

void PushingAsyncPipelineExecutor::finish()
{
    if (finished)
        return;
    finished = true;

    pushing_source->finish();

    /// Join thread here to wait for possible exception.
    if (data && data->thread.joinable())
        data->thread.join();

    /// Rethrow exception to not swallow it in destructor.
    if (data)
        data->rethrowExceptionIfHas();
}

void PushingAsyncPipelineExecutor::cancel()
{
    /// Cancel execution if it wasn't finished.
    if (data && !data->is_finished && data->executor)
        data->executor->cancel();

    finish();
}

}
