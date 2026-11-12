#include "astra-sim/common/Logging.hh"

namespace AstraSim {

std::unordered_set<spdlog::sink_ptr> LoggerFactory::default_sinks;

std::shared_ptr<spdlog::logger> LoggerFactory::get_logger(
    const std::string& logger_name) {
    constexpr bool ENABLE_DEFAULT_SINK_FOR_OTHER_LOGGERS = true;
    auto logger = spdlog::get(logger_name);
    if (logger == nullptr) {
        logger = spdlog::create_async<spdlog::sinks::null_sink_mt>(logger_name);
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::info);
    }
    if constexpr (!ENABLE_DEFAULT_SINK_FOR_OTHER_LOGGERS) {
        return logger;
    }
    auto& logger_sinks = logger->sinks();
    for (auto sink : default_sinks) {
        if (std::find(logger_sinks.begin(), logger_sinks.end(), sink) ==
            logger_sinks.end()) {
            logger_sinks.push_back(sink);
        }
    }
    return logger;
}

void LoggerFactory::init(const std::string& log_config_path,
                         bool console_to_stderr) {
    if (log_config_path != "empty") {
        spdlog_setup::from_file(log_config_path);
        if (console_to_stderr) {
            // Configured stdout sinks must not corrupt the controller wire
            // either. Keep file sinks and redirect console diagnostics only.
            spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
                for (auto& sink : logger->sinks()) {
                    if (std::dynamic_pointer_cast<spdlog::sinks::stdout_color_sink_mt>(sink) ||
                        std::dynamic_pointer_cast<spdlog::sinks::stdout_sink_mt>(sink)) {
                        auto replacement = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                        replacement->set_level(sink->level());
                        sink = replacement;
                    }
                }
            });
        }
    }
    init_default_components(console_to_stderr);
}

void LoggerFactory::shutdown(void) {
    default_sinks.clear();
    spdlog::drop_all();
    spdlog::shutdown();
}

void LoggerFactory::init_default_components(bool console_to_stderr) {
    // READY/TEMPLATE/COMPLETE records use stdout. An asynchronous stdout
    // logger can interleave inside a chained cout record and deadlock both
    // sides. Legacy prose handshakes still require stdout logging.
    spdlog::sink_ptr sink_color_console;
    if (console_to_stderr) {
        sink_color_console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    } else {
        sink_color_console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    }
    sink_color_console->set_level(spdlog::level::info);
    default_sinks.insert(sink_color_console);

    auto sink_rotate_out =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "log/log.log", 1024 * 1024 * 10, 10);
    sink_rotate_out->set_level(spdlog::level::debug);
    default_sinks.insert(sink_rotate_out);

    auto sink_rotate_err =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "log/err.log", 1024 * 1024 * 10, 10);
    sink_rotate_err->set_level(spdlog::level::err);
    default_sinks.insert(sink_rotate_err);

    spdlog::init_thread_pool(8192, 1);
    spdlog::set_pattern("[%Y-%m-%dT%T%z] [%L] <%n>: %v");
}

}  // namespace AstraSim
