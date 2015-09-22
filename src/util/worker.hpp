#pragma once

#include <condition_variable>
#include <thread>
#include <queue>

#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/locks.hpp"

template<typename T,
         typename Q = std::queue<T>>
class TWorker : public TLockable {
protected:
    volatile bool Valid = true;
    Q Queue;
    std::condition_variable Cv;
    std::vector<std::shared_ptr<std::thread>> Threads;
    size_t Seq = 0;
    const std::string Name;
    const size_t Nr;
public:
    TWorker(const std::string &name, size_t nr) : Name(name), Nr(nr) {}

    void Start() {
        for (size_t i = 0; i < Nr; i++)
            Threads.push_back(std::make_shared<std::thread>(&TWorker::WorkerFn, this, Name + std::to_string(i)));
    }

    void Stop() {
        if (Valid) {
            {
                auto lock = ScopedLock();
                Valid = false;
                Cv.notify_all();
            }
            for (auto thread : Threads)
                thread->join();
            Threads.clear();
        }
    }

    void Push(const T &elem) {
        auto lock = ScopedLock();
        Queue.push(elem);
        Seq++;
        Cv.notify_one();
    }

    virtual void Wait(TScopedLock &lock) {
        if (!Valid)
            return;

        Cv.wait(lock);
    }

    void WorkerFn(const std::string &name) {
        try {
            BlockAllSignals();
            if (!config().daemon().debug())
                RegisterSignal(SIGSEGV, DumpStackAndDie);
            SetProcessName(name);
            auto lock = ScopedLock();
            while (Valid) {
                if (Queue.empty())
                    Wait(lock);

                while (Valid && !Queue.empty()) {
                    T request = Top();
                    Queue.pop();

                    size_t seq = Seq;
                    lock.unlock();
                    bool handled = Handle(request);
                    lock.lock();
                    bool haveNewData = seq != Seq;

                    if (!handled) {
                        Queue.push(request);
                        if (!haveNewData)
                            Wait(lock);
                    }
                }
            }
        } catch (std::string s) {
            if (config().daemon().debug())
                throw;
            L_ERR() << "EXCEPTION: " << s << std::endl;
            Crash();
        } catch (const char *s) {
            if (config().daemon().debug())
                throw;
            L_ERR() << "EXCEPTION: " << s << std::endl;
            Crash();
        } catch (const std::exception &exc) {
            if (config().daemon().debug())
                throw;
            L_ERR() << "EXCEPTION: " << exc.what() << std::endl;
            Crash();
        } catch (...) {
            if (config().daemon().debug())
                throw;
            L_ERR() << "EXCEPTION: uncaught exception!" << std::endl;
            Crash();
        }
    }

    virtual const T &Top() =0;
    virtual bool Handle(const T &elem) =0;
};