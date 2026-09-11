#pragma once

#include <concepts>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "EventFormatter.h"
#include "Events.h"

// T is an event value (EventVariant by default), carried through the queue by
// copy rather than behind a pointer. wait_and_pop() returns void on LockQueue, so
// this only requires the call to be well-formed, not to yield anything.
// push() is required as well: stop() enqueues a default-constructed T as the
// shutdown sentinel, and is_shutdown() is how the drain loop recognises it --
// the pair that replaced "push nullptr, test for null".
template<typename Cont, typename T>
concept validInputCont = requires (Cont cont, T value, std::shared_ptr<std::ofstream> file) {
    cont.wait_and_pop(value);
    cont.push(T{});
    { is_shutdown(value) } -> std::same_as<bool>;
    write_event(value, file);
};

// InputCont is a template template parameter because the class stores an
// InputCont<T>; a plain `typename` could not be written as InputCont<T>.
template<template<typename> class InputCont, typename T = EventVariant>
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

            // Hoisted out of the loop: default-constructing T per iteration would
            // build and throw away a sentinel every time round.
            T log_entry;
            while (true) {
                input_container->wait_and_pop(log_entry);
                if (is_shutdown(log_entry)) break;  // no more events are coming
                if (writable) write_event(log_entry, output_file);
            }

            if (writable) {
                output_file->flush();
                output_file->close();
            }
        }

        // Called from the producer side once it is done. Enqueuing a
        // default-constructed entry (the Shutdown alternative) wakes
        // readWriteLogs() out of its blocking pop; every event queued before it is
        // still drained first, so no log line is lost.
        void stop() {
            input_container->push(T{});
        }
};
