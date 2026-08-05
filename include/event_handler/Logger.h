#pragma once

#include <concepts>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "Events.h"

// T is a pointer-like handle to an Event (unique_ptr<Event> by default), so the
// logger dispatches through `->`. wait_and_pop() returns void on LockQueue, so
// this only requires the call to be well-formed, not to yield anything.
// push() is required as well: stop() enqueues a null T as the shutdown sentinel.
template<typename Cont, typename T>
concept validInputCont = requires (Cont cont, T value, std::shared_ptr<std::ofstream> file) {
    cont.wait_and_pop(value);
    cont.push(T{});
    value->push_to_file(file);
};

// InputCont is a template template parameter because the class stores an
// InputCont<T>; a plain `typename` could not be written as InputCont<T>.
template<template<typename> class InputCont, typename T = std::unique_ptr<Event>>
requires validInputCont<InputCont<T>, T>
class Logger {
    private:
        std::string log_file;
        std::shared_ptr<InputCont<T>> input_container;
        std::shared_ptr<std::ofstream> output_file;
    public:
        Logger(std::shared_ptr<InputCont<T>> input_container, std::string log_file_name)
            : log_file(std::move(log_file_name)), input_container(input_container) {}
        ~Logger() = default;

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;
        Logger(Logger&&) = delete;
        Logger& operator=(Logger&&) = delete;

        // Runs on the logger thread. Blocks on the queue rather than polling, so
        // the matching thread never waits on file I/O. Returns once the sentinel
        // pushed by stop() is popped.
        void readWriteLogs() {
            output_file = std::make_shared<std::ofstream>();
            output_file->open(log_file, std::ios::out);

            // A failed open leaves the stream in a bad state where every write is
            // silently discarded, so say so once rather than losing the whole log
            // without a word. The queue is still drained: the producer must not be
            // left waiting on a consumer that quit early.
            const bool writable = output_file->is_open();
            if (!writable) {
                std::cerr << "Logger: cannot open '" << log_file
                          << "' — events will be dropped\n";
            }

            while (true) {
                T log_entry;
                input_container->wait_and_pop(log_entry);
                if (!log_entry) break;  // sentinel: no more events are coming
                if (writable) log_entry->push_to_file(output_file);
            }

            if (writable) {
                output_file->flush();
                output_file->close();
            }
        }

        // Called from the producer side once it is done. Enqueuing a null entry
        // wakes readWriteLogs() out of its blocking pop; every event queued
        // before it is still drained first, so no log line is lost.
        void stop() {
            input_container->push(T{});
        }
};
