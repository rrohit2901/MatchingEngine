#pragma once

#include <iostream>
#include <concepts>
#include <memory>

template<typename InputCont, typename T>
concept validInputCont = requires (InputCont cont, T value, T& result, std::shared_ptr<std::ofstream> file) {
    {cont.wait_and_pop(result)} -> std::convertible_to<bool>;
    value.push_to_file(file);
};

template<typename InputCont, typename T>
requires validInputCont<InputCont, T>
class Logger {
    private:
        std::string log_file;
        std::shared_ptr<InputCont<T>> input_container;
    public:
        Logger(std::shared_ptr<InputCont<T>> input_container, std::string log_file_name): log_file(log_file), input_container{input_container} {}
        ~Logger() = default;

        Logger(const Logger&) = delete;
        Logger& operator()(const Logger&) = delete;
        Logger(Logger&&) = delete;
        Logger& operator()(Logger&&) =  delete;

        void readWriteLogs() {
            output_file = std::make_shared<std::ofstream>();
            output_file->open(log_file, std::ios::out);
            while(true) {
                T log_entry;
                bool pop_value = input_container->wait_and_pop(log_entry);
                if(!log_entry) break;
                if (pop_value) {
                    log_entry.push_to_file(log_file);
                }
            }
        }
};
