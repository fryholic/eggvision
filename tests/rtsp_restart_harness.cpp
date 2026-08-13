#include "bsaps/camera_capture.hpp"
#include "bsaps/config.hpp"
#include "bsaps/metrics.hpp"
#include "bsaps/rtsp_server.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace {

constexpr GstClockTime kProbeTimeout = 10 * GST_SECOND;

bool receiveRtp(const std::string &url) {
    const std::string pipeline_description =
        "rtspsrc location=" + url +
        " protocols=tcp latency=0 timeout=5000000 "
        "! application/x-rtp,media=video,encoding-name=H264 "
        "! rtph264depay ! h264parse "
        "! appsink name=probe sync=false max-buffers=1 drop=true";
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_description.c_str(), &error);
    if (!pipeline) {
        std::cerr << "[restart-test] client pipeline creation failed: "
                  << (error && error->message ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        return false;
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "probe");
    const bool started = sink && gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
                                     GST_STATE_CHANGE_FAILURE;
    GstSample *sample = started
                            ? gst_app_sink_try_pull_sample(GST_APP_SINK(sink), kProbeTimeout)
                            : nullptr;
    if (!sample) {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = gst_bus_pop_filtered(
            bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *client_error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &client_error, &debug);
            std::cerr << "[restart-test] RTP probe failed: "
                      << (client_error && client_error->message
                              ? client_error->message
                              : "unknown client error")
                      << '\n';
            g_clear_error(&client_error);
            g_free(debug);
        }
        if (message) {
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    }
    if (sample) {
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (sink) {
        gst_object_unref(sink);
    }
    gst_object_unref(pipeline);
    g_clear_error(&error);
    return sample != nullptr;
}

void configureFailure(const std::string &stage) {
    constexpr const char *variables[] = {
        "BSAPS_RTSP_TEST_FAIL_RECOVERY_THREAD_CREATE",
        "BSAPS_RTSP_TEST_FAIL_CLEANUP_THREAD_CREATE",
        "BSAPS_RTSP_TEST_FAIL_FEEDER_THREAD_CREATE",
        "BSAPS_RTSP_TEST_FAIL_LOOP_THREAD_CREATE",
    };
    for (const char *variable : variables) {
        g_unsetenv(variable);
    }
    if (stage == "none") {
        return;
    }
    const char *variable = nullptr;
    if (stage == "recovery") {
        variable = variables[0];
    } else if (stage == "cleanup") {
        variable = variables[1];
    } else if (stage == "feeder") {
        variable = variables[2];
    } else if (stage == "loop") {
        variable = variables[3];
    } else {
        throw std::runtime_error("unknown failure stage: " + stage);
    }
    g_setenv(variable, "1", TRUE);
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const std::string failure_stage = argc > 1 ? argv[1] : "none";
        const std::string port = argc > 2 ? argv[2] : "8554";
        const unsigned target_epochs = argc > 3
                                           ? static_cast<unsigned>(std::stoul(argv[3]))
                                           : (failure_stage == "none" ? 2U : 1U);
        if (target_epochs == 0) {
            throw std::runtime_error("epoch count must be positive");
        }
        configureFailure(failure_stage);

        bsaps::AppConfig config;
        config.inference_enabled = false;
        config.rtsp_address = "127.0.0.1";
        config.rtsp_port = port;
        bsaps::Metrics metrics;
        bsaps::RtspServer server(config, metrics);
        bsaps::CameraCapture camera(config, metrics);
        if (!camera.initialize()) {
            throw std::runtime_error("camera initialization failed");
        }
        camera.setMainConsumer([&server](std::shared_ptr<bsaps::FrameLease> frame) {
            server.submit(std::move(frame));
        });
        if (!camera.start()) {
            throw std::runtime_error("camera start failed");
        }

        const std::string url = server.url("127.0.0.1");
        unsigned successful_epochs = 0;
        if (failure_stage != "none") {
            if (server.start()) {
                throw std::runtime_error("fault-injected start unexpectedly succeeded");
            }
            // stop() is intentionally idempotent after start() has rolled back.
            server.stop();
            std::cout << "[restart-test] rollback stage=" << failure_stage
                      << " complete; retrying same object\n";
        }

        bool next_epoch_already_started = false;
        for (unsigned epoch = 1; epoch <= target_epochs; ++epoch) {
            if (epoch == 1) {
                std::atomic<bool> go{false};
                bool first = false;
                bool second = false;
                auto start_concurrently = [&server, &go](bool &result) {
                    while (!go.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    result = server.start();
                };
                std::thread left(start_concurrently, std::ref(first));
                std::thread right(start_concurrently, std::ref(second));
                go.store(true, std::memory_order_release);
                left.join();
                right.join();
                if (first == second) {
                    throw std::runtime_error(
                        "concurrent start calls did not produce one owner and one rejection");
                }
                std::cout << "[restart-test] concurrent start serialized\n";
            } else if (!next_epoch_already_started && !server.start()) {
                throw std::runtime_error("same-object start failed at epoch " +
                                         std::to_string(epoch));
            }
            next_epoch_already_started = false;
            if (server.start()) {
                throw std::runtime_error("concurrent/running start was not rejected");
            }
            if (!receiveRtp(url)) {
                throw std::runtime_error("RTP timeout at epoch " + std::to_string(epoch));
            }
            ++successful_epochs;
            std::cout << "[restart-test] epoch=" << epoch << " RTP received\n";
            if (epoch != target_epochs) {
                std::atomic<bool> go{false};
                bool restart_won = false;
                std::thread stopper([&server, &go] {
                    while (!go.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    server.stop();
                });
                std::thread starter([&server, &go, &restart_won] {
                    while (!go.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    restart_won = server.start();
                });
                go.store(true, std::memory_order_release);
                stopper.join();
                starter.join();
                next_epoch_already_started = restart_won;
                std::cout << "[restart-test] concurrent stop/start serialized restart="
                          << (restart_won ? "yes" : "no") << '\n';
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        camera.stop();
        server.stop();
        if (metrics.outstanding_leases.load() != 0) {
            throw std::runtime_error("outstanding leases remain: " +
                                     std::to_string(metrics.outstanding_leases.load()));
        }
        std::cout << "[restart-test] passed stage=" << failure_stage
                  << " successful_epochs=" << successful_epochs
                  << " outstanding=0 rtsp_errors=" << metrics.rtsp_errors.load()
                  << " recovery_failures=" << metrics.rtsp_recovery_failures.load() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[restart-test] FAIL: " << error.what() << '\n';
        return 1;
    }
}
