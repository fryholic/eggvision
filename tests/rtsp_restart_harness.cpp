#include "eggvision/camera_capture.hpp"
#include "eggvision/config.hpp"
#include "eggvision/h264_encoder.hpp"
#include "eggvision/metrics.hpp"
#include "eggvision/rtsp_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace {

constexpr GstClockTime kProbeTimeout = 10 * GST_SECOND;

bool receiveRtp(const std::string &url, GstElement **held_pipeline = nullptr) {
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
    if (held_pipeline && sample) {
        *held_pipeline = pipeline;
    } else {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (sink) {
        gst_object_unref(sink);
    }
    if (!held_pipeline || !sample) {
        gst_object_unref(pipeline);
    }
    g_clear_error(&error);
    return sample != nullptr;
}

void stopRtp(GstElement *pipeline) {
    if (!pipeline) {
        return;
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

void configureFailure(const std::string &stage) {
    constexpr const char *variables[] = {
        "EGGVISION_RTSP_TEST_FAIL_RECOVERY_THREAD_CREATE",
        "EGGVISION_RTSP_TEST_FAIL_CLEANUP_THREAD_CREATE",
        "EGGVISION_RTSP_TEST_FAIL_FEEDER_THREAD_CREATE",
        "EGGVISION_RTSP_TEST_FAIL_LOOP_THREAD_CREATE",
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

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const std::string failure_stage = argc > 1 ? argv[1] : "none";
        const std::string port = argc > 2 ? argv[2] : "8554";
        const unsigned target_epochs = argc > 3
                                           ? static_cast<unsigned>(std::stoul(argv[3]))
                                           : (failure_stage == "none" ? 2U : 1U);
        const unsigned delayed_recovery_ms = argc > 4
                                                 ? static_cast<unsigned>(std::stoul(argv[4]))
                                                 : 0U;
        if (target_epochs == 0 || (delayed_recovery_ms != 0 && target_epochs < 2)) {
            throw std::runtime_error(
                "epoch count must be positive and delayed recovery requires two epochs");
        }
        configureFailure(failure_stage);
        const std::string recovery_trigger =
            "/tmp/eggvision-rtsp-restart-" + port + ".trigger";
        std::remove(recovery_trigger.c_str());
        if (delayed_recovery_ms != 0) {
            g_setenv("EGGVISION_RTSP_TEST_PUSH_ERROR_TRIGGER", recovery_trigger.c_str(), TRUE);
            g_setenv("EGGVISION_RTSP_TEST_TEARDOWN_DELAY_MS",
                     std::to_string(delayed_recovery_ms).c_str(),
                     TRUE);
        } else {
            g_unsetenv("EGGVISION_RTSP_TEST_PUSH_ERROR_TRIGGER");
            g_unsetenv("EGGVISION_RTSP_TEST_TEARDOWN_DELAY_MS");
        }

        eggvision::AppConfig config;
        config.inference_enabled = false;
        config.rtsp_address = "127.0.0.1";
        config.rtsp_port = port;
        eggvision::Metrics metrics;
        eggvision::RtspServer server(config, metrics);
        eggvision::H264Encoder encoder(config, metrics);
        eggvision::CameraCapture camera(config, metrics);
        if (!encoder.initialize() || !camera.initialize()) {
            throw std::runtime_error("encoder or camera initialization failed");
        }
        encoder.setConsumer([&server](eggvision::EncodedAccessUnitPtr unit) {
            server.submit(std::move(unit));
        });
        camera.setMainConsumer([&encoder](std::shared_ptr<eggvision::FrameLease> frame) {
            encoder.submit(std::move(frame));
        });
        if (!encoder.start() || !camera.start()) {
            throw std::runtime_error("encoder or camera start failed");
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
            GstElement *held_client = nullptr;
            if (!receiveRtp(url,
                            epoch == 1 && delayed_recovery_ms != 0
                                ? &held_client
                                : nullptr)) {
                throw std::runtime_error("RTP timeout at epoch " + std::to_string(epoch));
            }
            ++successful_epochs;
            std::cout << "[restart-test] epoch=" << epoch << " RTP received\n";
            if (epoch == 1 && delayed_recovery_ms != 0) {
                if (!g_file_set_contents(recovery_trigger.c_str(), "", 0, nullptr)) {
                    throw std::runtime_error("failed to create recovery trigger");
                }
                if (!waitUntil([&server] { return server.recoveryRunningForTest(); },
                               std::chrono::seconds(5))) {
                    stopRtp(held_client);
                    throw std::runtime_error("delayed recovery did not enter Running state");
                }

                const auto stop_started = std::chrono::steady_clock::now();
                std::thread stopper([&server] { server.stop(); });
                if (!waitUntil([&server] { return !server.runningForTest(); },
                               std::chrono::seconds(3))) {
                    stopper.join();
                    stopRtp(held_client);
                    throw std::runtime_error("stop did not publish the stopped state");
                }
                // start() must be serialized behind the still-running old
                // teardown. Remove the one-shot trigger before it can enter
                // the next epoch, then prove restart cannot overtake stop().
                std::remove(recovery_trigger.c_str());
                auto restart = std::async(std::launch::async, [&server] {
                    return server.start();
                });
                if (restart.wait_for(std::chrono::seconds(1)) !=
                    std::future_status::timeout) {
                    stopper.join();
                    stopRtp(held_client);
                    throw std::runtime_error(
                        "same-object restart overtook the previous recovery job");
                }
                stopper.join();
                stopRtp(held_client);
                if (!restart.get()) {
                    throw std::runtime_error("same-object restart failed after recovery join");
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - stop_started);
                const auto minimum = std::chrono::milliseconds(delayed_recovery_ms) -
                                     std::chrono::milliseconds(500);
                if (elapsed < minimum) {
                    throw std::runtime_error(
                        "stop returned before delayed recovery ownership was released");
                }
                next_epoch_already_started = true;
                std::cout << "[restart-test] delayed recovery joined before restart elapsed_ms="
                          << elapsed.count() << '\n';
                continue;
            }
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
        encoder.stop();
        server.stop();
        if (metrics.outstanding_leases.load() != 0) {
            throw std::runtime_error("outstanding leases remain: " +
                                     std::to_string(metrics.outstanding_leases.load()));
        }
        std::cout << "[restart-test] passed stage=" << failure_stage
                  << " successful_epochs=" << successful_epochs
                  << " delayed_recovery_ms=" << delayed_recovery_ms
                  << " outstanding=0 rtsp_errors=" << metrics.rtsp_errors.load()
                  << " recovery_failures=" << metrics.rtsp_recovery_failures.load() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[restart-test] FAIL: " << error.what() << '\n';
        return 1;
    }
}
