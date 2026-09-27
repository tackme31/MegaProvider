#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// One worker thread running jobs in submission order: the Qt-free replacement for
// MegaExplorer's QThreadPool capped at one thread (see MegaSdkClient::mListingPool).
class SerialExecutor
{
public:
    SerialExecutor() : mThread([this] { run(); }) {}

    ~SerialExecutor()
    {
        waitForDone();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
        }
        mWake.notify_all();
        mThread.join();
    }

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    void start(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mJobs.push_back(std::move(job));
        }
        mWake.notify_all();
    }

    // Blocks until the queue is empty and no job is running (QThreadPool::waitForDone).
    void waitForDone()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mIdle.wait(lock, [this] { return mJobs.empty() && !mBusy; });
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;)
        {
            mWake.wait(lock, [this] { return mStopping || !mJobs.empty(); });
            if (mJobs.empty())
                return; // stopping, and nothing left to run
            auto job = std::move(mJobs.front());
            mJobs.pop_front();
            mBusy = true;
            lock.unlock();
            job();
            lock.lock();
            mBusy = false;
            mIdle.notify_all();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mIdle;
    std::deque<std::function<void()>> mJobs;
    bool mBusy = false;
    bool mStopping = false;
    std::thread mThread; // last: started in the constructor once everything above exists
};
