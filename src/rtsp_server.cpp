#include "eggvision/rtsp_server.hpp"
#include "eggvision/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace eggvision {
namespace {

struct GstAppSrcUnref {
    void operator()(GstAppSrc *source) const {
        if (source) {
            gst_object_unref(source);
        }
    }
};

using GstAppSrcRef = std::unique_ptr<GstAppSrc, GstAppSrcUnref>;

void destroyEncodedUnit(gpointer data) {
    delete static_cast<EncodedAccessUnitPtr *>(data);
}

}  // namespace

struct RtspServer::CallbackState {
    std::mutex mutex;
    std::condition_variable cv;
    RtspServer *server = nullptr;
    unsigned active = 0;
};

struct RtspServer::RecoveryJob {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> done{false};
    std::uint64_t token = 0;
    GstRTSPServer *server = nullptr;
    GstRTSPSessionPool *pool = nullptr;
    GstRTSPMediaFactory *factory = nullptr;
    GstAppSrc *source = nullptr;
    unsigned teardown_delay_ms = 0;
};

struct RtspServer::RecoveryWorkerState {
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<RecoveryJob> pending;
    bool stopping = false;
};

struct RtspServer::SessionCleanupState {
    GstRTSPSessionPool *pool = nullptr;
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    GSource *source = nullptr;
    unsigned max_sessions = 0;
    unsigned test_delay_ms = 0;
    bool test_delay_consumed = false;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> current{0};
    std::atomic<std::uint64_t> cleaned{0};
};

RtspServer::CallbackGuard::CallbackGuard(gpointer user_data) {
    auto *holder = static_cast<std::shared_ptr<CallbackState> *>(user_data);
    if (!holder) {
        return;
    }
    state_ = *holder;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->server) {
        server_ = state_->server;
        ++state_->active;
    }
}

RtspServer::CallbackGuard::~CallbackGuard() {
    if (!server_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (--state_->active == 0) {
        state_->cv.notify_all();
    }
}

gpointer RtspServer::newCallbackData(const std::shared_ptr<CallbackState> &state) {
    return new std::shared_ptr<CallbackState>(state);
}

void RtspServer::destroySignalCallbackData(gpointer data, GClosure *) {
    delete static_cast<std::shared_ptr<CallbackState> *>(data);
}

void RtspServer::destroySourceCallbackData(gpointer data) {
    delete static_cast<std::shared_ptr<CallbackState> *>(data);
}

RtspServer::RtspServer(const AppConfig &config, Metrics &metrics)
    : config_(config), metrics_(metrics), callback_state_(std::make_shared<CallbackState>()) {
    callback_state_->server = this;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    test_appsrc_lifetime_ = std::make_shared<TestAppSrcLifetimeState>();
    // This hook is compiled out of normal builds. Test builds opt in through
    // EGGVISION_ENABLE_TEST_HOOKS and still remain inert unless the trigger path is
    // explicitly supplied in the environment.
    if (const gchar *trigger = g_getenv("EGGVISION_RTSP_TEST_PUSH_ERROR_TRIGGER")) {
        test_push_error_trigger_ = trigger;
    }
    if (const gchar *delay = g_getenv("EGGVISION_RTSP_TEST_TEARDOWN_DELAY_MS")) {
        gchar *end = nullptr;
        const guint64 parsed = g_ascii_strtoull(delay, &end, 10);
        if (end != delay && end && *end == '\0') {
            test_teardown_delay_ms_ = static_cast<unsigned>(
                std::min<guint64>(parsed, G_MAXUINT));
        }
    }
    auto parse_test_unsigned = [](const char *name) {
        const gchar *value = g_getenv(name);
        if (!value) {
            return 0U;
        }
        gchar *end = nullptr;
        const guint64 parsed = g_ascii_strtoull(value, &end, 10);
        return end != value && end && *end == '\0'
                   ? static_cast<unsigned>(std::min<guint64>(parsed, G_MAXUINT))
                   : 0U;
    };
    test_watchdog_recovery_delay_ms_ =
        parse_test_unsigned("EGGVISION_RTSP_TEST_WATCHDOG_RECOVERY_DELAY_MS");
    if (const gchar *pause = g_getenv("EGGVISION_RTSP_TEST_RECOVERY_PAUSE")) {
        test_recovery_pause_path_ = pause;
    }
    test_session_timeout_seconds_ =
        parse_test_unsigned("EGGVISION_RTSP_TEST_SESSION_TIMEOUT_SECONDS");
    test_session_cleanup_delay_ms_ =
        parse_test_unsigned("EGGVISION_RTSP_TEST_SESSION_CLEANUP_DELAY_MS");
    test_fail_recovery_thread_create_ =
        g_strcmp0(g_getenv("EGGVISION_RTSP_TEST_FAIL_RECOVERY_THREAD_CREATE"), "1") == 0;
    test_fail_cleanup_thread_create_ =
        g_strcmp0(g_getenv("EGGVISION_RTSP_TEST_FAIL_CLEANUP_THREAD_CREATE"), "1") == 0;
    test_fail_feeder_thread_create_ =
        g_strcmp0(g_getenv("EGGVISION_RTSP_TEST_FAIL_FEEDER_THREAD_CREATE"), "1") == 0;
    test_fail_loop_thread_create_ =
        g_strcmp0(g_getenv("EGGVISION_RTSP_TEST_FAIL_LOOP_THREAD_CREATE"), "1") == 0;
    test_force_initial_keyframe_drop_ =
        g_strcmp0(g_getenv("EGGVISION_RTSP_TEST_FORCE_INITIAL_KEYFRAME_DROP"), "1") == 0;
#endif
    gst_init(nullptr, nullptr);
}

RtspServer::~RtspServer() {
    stop();
}

#ifdef EGGVISION_ENABLE_TEST_HOOKS
void RtspServer::testAppSrcDestroyed(gpointer data, GObject *) {
    auto *state = static_cast<std::shared_ptr<TestAppSrcLifetimeState> *>(data);
    (*state)->destroyed.fetch_add(1, std::memory_order_release);
    delete state;
}

std::uint64_t RtspServer::appsrcBoundForTest() const {
    return test_appsrc_lifetime_->bound.load(std::memory_order_acquire);
}

std::uint64_t RtspServer::appsrcDestroyedForTest() const {
    return test_appsrc_lifetime_->destroyed.load(std::memory_order_acquire);
}
#endif

GSource *RtspServer::createListenerSource() {
    if (!server_) {
        return nullptr;
    }
    GError *error = nullptr;
    GSource *source = gst_rtsp_server_create_source(server_, nullptr, &error);
    if (!source) {
        synchronizedLog(std::cerr) << "[rtsp] failed to bind " << config_.rtsp_address << ':'
                  << config_.rtsp_port << ": "
                  << (error && error->message ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        return nullptr;
    }
    if (g_source_attach(source, nullptr) == 0) {
        synchronizedLog(std::cerr) << "[rtsp] failed to attach listener source\n";
        destroySource(source);
        return nullptr;
    }
    return source;
}

GSource *RtspServer::createWatchdogSource() {
    GSource *source = g_timeout_source_new(500);
    if (!source) {
        return nullptr;
    }
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source,
                          RtspServer::watchdogTick,
                          newCallbackData(callback_state_),
                          RtspServer::destroySourceCallbackData);
    if (g_source_attach(source, nullptr) == 0) {
        destroySource(source);
        return nullptr;
    }
    return source;
}

void RtspServer::destroySource(GSource *source) {
    if (!source) {
        return;
    }
    if (!g_source_is_destroyed(source)) {
        g_source_destroy(source);
    }
    g_source_unref(source);
}

bool RtspServer::installFactory() {
    if (!running_.load() || !mounts_) {
        return false;
    }

    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    std::ostringstream pipeline;
    pipeline << "( appsrc name=source is-live=true format=time do-timestamp=false block=false "
             << "max-buffers=1 leaky-type=downstream "
             << "! video/x-h264,stream-format=byte-stream,alignment=au,profile=high,level=(string)4 "
             << "! h264parse config-interval=1 "
             << "! rtph264pay name=pay0 pt=96 config-interval=1 mtu=1400 )";

    gst_rtsp_media_factory_set_launch(factory, pipeline.str().c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_stop_on_disconnect(factory, FALSE);
    gst_rtsp_media_factory_set_eos_shutdown(factory, FALSE);
    gst_rtsp_media_factory_set_suspend_mode(factory, GST_RTSP_SUSPEND_MODE_NONE);
    gst_rtsp_media_factory_set_protocols(
        factory, static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
    gst_rtsp_media_factory_set_latency(factory, 50);
    const gulong handler = g_signal_connect_data(
        factory,
        "media-configure",
        G_CALLBACK(RtspServer::mediaConfigure),
        newCallbackData(callback_state_),
        RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));

    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (!running_.load() || current_factory_ != nullptr) {
            g_signal_handler_disconnect(factory, handler);
            g_object_unref(factory);
            return false;
        }
        current_factory_ = static_cast<GstRTSPMediaFactory *>(g_object_ref(factory));
        current_factory_handler_ = handler;
    }
    gst_rtsp_mount_points_add_factory(mounts_, config_.rtsp_mount.c_str(), factory);
    return true;
}

bool RtspServer::start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load()) {
        synchronizedLog(std::cerr) << "[rtsp] start rejected: server is already running\n";
        return false;
    }

    // Every start publishes a fresh callback epoch. Signal/source closures from
    // a previous epoch retain only its disabled CallbackState and therefore
    // cannot re-enter this object after a restart.
    try {
        callback_state_ = std::make_shared<CallbackState>();
    } catch (const std::bad_alloc &error) {
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] failed to allocate callback epoch: " << error.what() << '\n';
        return false;
    }
    callback_state_->server = this;
    latest_.reopen();
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        running_.store(true);
        listener_source_ = nullptr;
        watchdog_source_ = nullptr;
        accepting_clients_ = false;
        appsrc_ = nullptr;
        current_media_ = nullptr;
        current_media_handlers_ = {};
        current_factory_ = nullptr;
        current_factory_handler_ = 0;
        ++media_generation_;
        consecutive_push_failures_ = 0;
        recovery_state_ = RecoveryState::Idle;
        recovery_generation_ = 0;
        recovery_token_ = 0;
        next_recovery_token_ = 0;
        recovery_reason_.clear();
        recovery_started_us_ = 0;
        recovery_failure_reported_ = false;
        recovery_media_unprepared_ = false;
        recovery_job_.reset();
        observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
        status_since_us_ = g_get_monotonic_time();
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        test_push_errors_remaining_ = 0;
        test_push_error_consumed_ = false;
        test_recovery_pause_reported_ = false;
#endif
    }

    loop_ = g_main_loop_new(nullptr, FALSE);
    server_ = gst_rtsp_server_new();
    if (!loop_ || !server_) {
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] failed to allocate server main-loop state\n";
        stopLocked();
        return false;
    }
    gst_rtsp_server_set_address(server_, config_.rtsp_address.c_str());
    gst_rtsp_server_set_service(server_, config_.rtsp_port.c_str());
    client_connected_handler_ = g_signal_connect_data(
        server_,
        "client-connected",
        G_CALLBACK(RtspServer::clientConnected),
        newCallbackData(callback_state_),
        RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    mounts_ = gst_rtsp_server_get_mount_points(server_);
    if (!installFactory()) {
        synchronizedLog(std::cerr) << "[rtsp] failed to install media factory\n";
        stopLocked();
        return false;
    }

    // Create the only recovery teardown worker before exposing the listener.
    // recoverMedia() runs behind a C callback and must never need to create a
    // thread after it has published Running state or transferred GObject refs.
    if (!startRecoveryWorker()) {
        synchronizedLog(std::cerr) << "[rtsp] failed to start recovery teardown worker\n";
        stopLocked();
        return false;
    }

    GSource *initial_listener = createListenerSource();
    if (!initial_listener) {
        stopLocked();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        listener_source_ = initial_listener;
        accepting_clients_ = true;
    }

    if (!startSessionCleanup()) {
        synchronizedLog(std::cerr) << "[rtsp] failed to start session cleanup owner\n";
        stopLocked();
        return false;
    }

    GSource *watchdog = createWatchdogSource();
    if (!watchdog) {
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] failed to attach watchdog source\n";
        stopLocked();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        watchdog_source_ = watchdog;
    }
    try {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (test_fail_feeder_thread_create_) {
            test_fail_feeder_thread_create_ = false;
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "test hook feeder thread creation failure");
        }
#endif
        feeder_thread_ = std::thread(&RtspServer::feederLoop, this);
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (test_fail_loop_thread_create_) {
            test_fail_loop_thread_create_ = false;
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "test hook loop thread creation failure");
        }
#endif
        loop_thread_ = std::thread([this] { g_main_loop_run(loop_); });
    } catch (const std::system_error &error) {
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] failed to start owner thread: " << error.what() << '\n';
        stopLocked();
        return false;
    }
    synchronizedLog(std::cout) << "[rtsp] ready at rtsp://<device-ip>:" << config_.rtsp_port
              << config_.rtsp_mount << " (pre-encoded H264 High@L4)\n";
    return true;
}

void RtspServer::submit(EncodedAccessUnitPtr unit) {
    if (!running_.load() || !unit) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (!appsrc_) {
            metrics_.rtsp_dropped.fetch_add(1);
            return;
        }
    }
    if (latest_.push(std::move(unit))) {
        metrics_.rtsp_dropped.fetch_add(1);
    }
}

void RtspServer::mediaConfigure(GstRTSPMediaFactory *factory,
                                GstRTSPMedia *media,
                                gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaConfigure(factory, media);
    }
}

void RtspServer::mediaPrepared(GstRTSPMedia *media, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaPrepared(media);
    }
}

void RtspServer::mediaUnprepared(GstRTSPMedia *media, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaUnprepared(media);
    }
}

void RtspServer::mediaTargetState(GstRTSPMedia *media, GstState state, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaTargetState(media, state);
    }
}

void RtspServer::mediaNewState(GstRTSPMedia *media, GstState state, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaNewState(media, state);
    }
}

gboolean RtspServer::mediaHandleMessage(GstRTSPMedia *media,
                                        GstMessage *message,
                                        gpointer user_data) {
    CallbackGuard guard(user_data);
    return guard.server() ? guard.server()->onMediaHandleMessage(media, message) : FALSE;
}

gboolean RtspServer::watchdogTick(gpointer user_data) {
    CallbackGuard guard(user_data);
    return guard.server() ? guard.server()->onWatchdog() : G_SOURCE_REMOVE;
}

void RtspServer::clientConnected(GstRTSPServer *,
                                 GstRTSPClient *client,
                                 gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onClientConnected(client);
    }
}

void RtspServer::clientNewSession(GstRTSPClient *,
                                  GstRTSPSession *session,
                                  gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onClientNewSession(session);
    }
}

gboolean RtspServer::cleanupSessions(GstRTSPSessionPool *pool, gpointer user_data) {
    auto *state = static_cast<SessionCleanupState *>(user_data);
    if (!state || state->stopping.load(std::memory_order_acquire)) {
        return G_SOURCE_REMOVE;
    }
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    if (!state->test_delay_consumed && state->test_delay_ms != 0) {
        state->test_delay_consumed = true;
        synchronizedLog(std::cerr) << "[rtsp] test hook delaying session cleanup by "
                  << state->test_delay_ms << "ms\n";
        g_usleep(static_cast<gulong>(state->test_delay_ms) * 1000);
    }
#endif
    const guint removed = gst_rtsp_session_pool_cleanup(pool);
    const guint active = gst_rtsp_session_pool_get_n_sessions(pool);
    state->current.store(active);
    state->cleaned.fetch_add(removed);
    if (removed != 0) {
        synchronizedLog(std::cerr) << "[rtsp] session cleanup removed=" << removed
                  << " active=" << active << " max=" << state->max_sessions << '\n';
    }
    return state->stopping.load(std::memory_order_acquire)
               ? G_SOURCE_REMOVE
               : G_SOURCE_CONTINUE;
}

void RtspServer::onClientConnected(GstRTSPClient *client) {
    g_signal_connect_data(client,
                          "new-session",
                          G_CALLBACK(RtspServer::clientNewSession),
                          newCallbackData(callback_state_),
                          RtspServer::destroySignalCallbackData,
                          static_cast<GConnectFlags>(0));
}

void RtspServer::onClientNewSession(GstRTSPSession *session) {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    if (test_session_timeout_seconds_ != 0) {
        gst_rtsp_session_set_timeout(session, test_session_timeout_seconds_);
        g_object_set(session, "extra-timeout", 0U, nullptr);
    }
#else
    (void)session;
#endif
    // GstRTSPSessionPool's 1.22 watch sleeps indefinitely when prepare() sees
    // an empty pool. Session insertion does not wake the attached context, so
    // explicitly wake our dedicated owner to make it recompute the new
    // session's deadline.
    std::shared_ptr<SessionCleanupState> cleanup_state;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        cleanup_state = session_cleanup_state_;
    }
    if (cleanup_state) {
        g_main_context_wakeup(cleanup_state->context);
    }
    GstRTSPSessionPool *pool = server_ ? gst_rtsp_server_get_session_pool(server_) : nullptr;
    if (!pool) {
        return;
    }
    const guint active = gst_rtsp_session_pool_get_n_sessions(pool);
    g_object_unref(pool);
    metrics_.rtsp_sessions_current.store(active);
    if (cleanup_state) {
        cleanup_state->current.store(active);
    }
    std::uint64_t peak = metrics_.rtsp_sessions_peak.load();
    while (active > peak &&
           !metrics_.rtsp_sessions_peak.compare_exchange_weak(peak, active)) {
    }
}

bool RtspServer::startSessionCleanup() {
    if (!server_ || session_cleanup_state_) {
        return false;
    }
    auto state = std::make_shared<SessionCleanupState>();
    state->pool = gst_rtsp_server_get_session_pool(server_);
    state->context = g_main_context_new();
    state->loop = state->context ? g_main_loop_new(state->context, FALSE) : nullptr;
    state->max_sessions = config_.rtsp_max_sessions;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    state->test_delay_ms = test_session_cleanup_delay_ms_;
#endif
    if (!state->pool || !state->context || !state->loop) {
        if (state->loop) {
            g_main_loop_unref(state->loop);
        }
        if (state->context) {
            g_main_context_unref(state->context);
        }
        if (state->pool) {
            g_object_unref(state->pool);
        }
        return false;
    }
    gst_rtsp_session_pool_set_max_sessions(state->pool, state->max_sessions);
    state->source = gst_rtsp_session_pool_create_watch(state->pool);
    if (!state->source) {
        g_main_loop_unref(state->loop);
        g_main_context_unref(state->context);
        g_object_unref(state->pool);
        return false;
    }
    g_source_set_callback(state->source,
                          G_SOURCE_FUNC(RtspServer::cleanupSessions),
                          state.get(),
                          nullptr);
    if (g_source_attach(state->source, state->context) == 0) {
        g_source_unref(state->source);
        g_main_loop_unref(state->loop);
        g_main_context_unref(state->context);
        g_object_unref(state->pool);
        return false;
    }
    std::thread owner;
    try {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (test_fail_cleanup_thread_create_) {
            test_fail_cleanup_thread_create_ = false;
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "test hook cleanup thread creation failure");
        }
#endif
        owner = std::thread([state] {
            g_main_loop_run(state->loop);
            // The cleanup owner is independent of RtspServer. It may finish a
            // slow session-removed/unprepare after application stop without
            // touching the server or Metrics, then releases its own refs.
            g_source_unref(state->source);
            g_main_loop_unref(state->loop);
            g_main_context_unref(state->context);
            g_object_unref(state->pool);
        });
        owner.detach();
    } catch (const std::system_error &error) {
        // If creation failed there is no owner. If detach itself failed, stop
        // and join the local owner before releasing its external state.
        state->stopping.store(true, std::memory_order_release);
        g_source_destroy(state->source);
        g_main_loop_quit(state->loop);
        g_main_context_wakeup(state->context);
        if (owner.joinable()) {
            owner.join();
        } else {
            g_source_unref(state->source);
            g_main_loop_unref(state->loop);
            g_main_context_unref(state->context);
            g_object_unref(state->pool);
        }
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] session cleanup thread creation failed: "
                  << error.what() << '\n';
        return false;
    }
    session_cleanup_state_ = state;
    synchronizedLog(std::cout) << "[rtsp] session pool cleanup ready max=" << state->max_sessions << '\n';
    return true;
}

bool RtspServer::startRecoveryWorker() {
    try {
        auto state = std::make_shared<RecoveryWorkerState>();
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (test_fail_recovery_thread_create_) {
            test_fail_recovery_thread_create_ = false;
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "test hook recovery thread creation failure");
        }
#endif
        std::thread worker([state] { RtspServer::recoveryWorkerLoop(state); });
        recovery_worker_state_ = std::move(state);
        recovery_worker_thread_ = std::move(worker);
        return true;
    } catch (const std::system_error &error) {
        metrics_.rtsp_errors.fetch_add(1);
        metrics_.rtsp_recovery_failures.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] recovery thread creation failed: " << error.what() << '\n';
        return false;
    } catch (const std::bad_alloc &error) {
        metrics_.rtsp_errors.fetch_add(1);
        metrics_.rtsp_recovery_failures.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] recovery worker allocation failed: " << error.what() << '\n';
        return false;
    }
}

void RtspServer::recoveryWorkerLoop(
    const std::shared_ptr<RecoveryWorkerState> &state) {
    while (true) {
        std::shared_ptr<RecoveryJob> job;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&state] { return state->stopping || state->pending; });
            if (!state->pending) {
                if (state->stopping) {
                    return;
                }
                continue;
            }
            job = std::move(state->pending);
        }

        if (job->teardown_delay_ms != 0) {
            g_usleep(static_cast<gulong>(job->teardown_delay_ms) * 1000);
        }
        if (job->source) {
            gst_app_src_end_of_stream(job->source);
            gst_object_unref(job->source);
            job->source = nullptr;
        }
        if (job->server) {
            GList *clients = gst_rtsp_server_client_filter(
                job->server, RtspServer::closeClient, nullptr);
            g_list_free_full(clients, g_object_unref);
        }
        if (job->pool) {
            GList *sessions = gst_rtsp_session_pool_filter(
                job->pool, RtspServer::removeSession, nullptr);
            g_list_free_full(sessions, g_object_unref);
            g_object_unref(job->pool);
            job->pool = nullptr;
        }
        if (job->server) {
            g_object_unref(job->server);
            job->server = nullptr;
        }
        if (job->factory) {
            g_object_unref(job->factory);
            job->factory = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->done.store(true, std::memory_order_release);
        }
        job->cv.notify_all();
    }
}

void RtspServer::requestRecoveryWorkerStop() {
    std::shared_ptr<RecoveryWorkerState> state;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        state = std::move(recovery_worker_state_);
    }
    if (state) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stopping = true;
        }
        state->cv.notify_all();
    }
    if (!recovery_worker_thread_.joinable()) {
        return;
    }
    // RecoveryJob's GObject graph can own compressed GstBuffers and callbacks,
    // so the worker remains part of this object's shutdown graph and must not
    // outlive stop() or the next start epoch.
    recovery_worker_thread_.join();
}

void RtspServer::requestSessionCleanupStop() {
    std::shared_ptr<SessionCleanupState> state;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        state = session_cleanup_state_;
        session_cleanup_state_.reset();
    }
    if (!state) {
        return;
    }
    metrics_.rtsp_sessions_current.store(state->current.load());
    metrics_.rtsp_sessions_cleaned.store(state->cleaned.load());
    state->stopping.store(true, std::memory_order_release);
    g_source_destroy(state->source);
    g_main_loop_quit(state->loop);
}

GstRTSPFilterResult RtspServer::closeClient(GstRTSPServer *,
                                             GstRTSPClient *client,
                                             gpointer) {
    // GST_RTSP_FILTER_REMOVE makes gst_rtsp_server_client_filter() close the
    // client after releasing the server lock. Do not close it a second time in
    // this callback.
    (void)client;
    return GST_RTSP_FILTER_REMOVE;
}

GstRTSPFilterResult RtspServer::removeSession(GstRTSPSessionPool *,
                                               GstRTSPSession *,
                                               gpointer) {
    return GST_RTSP_FILTER_REMOVE;
}

RtspServer::MediaHandlers RtspServer::connectMediaHandlers(GstRTSPMedia *media) {
    MediaHandlers handlers;
    handlers.prepared = g_signal_connect_data(
        media, "prepared", G_CALLBACK(RtspServer::mediaPrepared),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    handlers.unprepared = g_signal_connect_data(
        media, "unprepared", G_CALLBACK(RtspServer::mediaUnprepared),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        G_CONNECT_AFTER);
    handlers.target_state = g_signal_connect_data(
        media, "target-state", G_CALLBACK(RtspServer::mediaTargetState),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    handlers.new_state = g_signal_connect_data(
        media, "new-state", G_CALLBACK(RtspServer::mediaNewState),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    handlers.handle_message = g_signal_connect_data(
        media, "handle-message", G_CALLBACK(RtspServer::mediaHandleMessage),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    return handlers;
}

void RtspServer::disconnectMediaHandlers(GstRTSPMedia *media,
                                         const MediaHandlers &handlers) {
    if (!media) {
        return;
    }
    for (const gulong handler : {handlers.prepared,
                                 handlers.unprepared,
                                 handlers.target_state,
                                 handlers.new_state,
                                 handlers.handle_message}) {
        if (handler != 0 && g_signal_handler_is_connected(media, handler)) {
            g_signal_handler_disconnect(media, handler);
        }
    }
}

bool RtspServer::bindMediaSource(GstRTSPMedia *media) {
    GstElement *element = gst_rtsp_media_get_element(media);
    if (!element) {
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] media pipeline has no root element\n";
        return false;
    }
    GstElement *source = gst_bin_get_by_name_recurse_up(GST_BIN(element), "source");
    gst_object_unref(element);
    if (!source || !GST_IS_APP_SRC(source)) {
        if (source) {
            gst_object_unref(source);
        }
        metrics_.rtsp_errors.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] media pipeline has no appsrc\n";
        return false;
    }

    g_object_set(source,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", FALSE,
                 "block", FALSE,
                 nullptr);
    gst_app_src_set_stream_type(GST_APP_SRC(source), GST_APP_STREAM_TYPE_STREAM);
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
                                        "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au",
                                        "profile", G_TYPE_STRING, "high",
                                        "level", G_TYPE_STRING, "4",
                                        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);

    GstAppSrc *previous = nullptr;
    bool changed = false;
    bool accepted = true;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media ||
            ((recovery_state_ == RecoveryState::Requested ||
              recovery_state_ == RecoveryState::Running) &&
             recovery_generation_ == media_generation_)) {
            accepted = false;
        } else if (appsrc_ != GST_APP_SRC(source)) {
            previous = appsrc_;
            appsrc_ = GST_APP_SRC(source);  // gst_bin_get_by_name supplied this reference.
#ifdef EGGVISION_ENABLE_TEST_HOOKS
            test_appsrc_lifetime_->bound.fetch_add(1, std::memory_order_release);
            g_object_weak_ref(
                G_OBJECT(source),
                RtspServer::testAppSrcDestroyed,
                new std::shared_ptr<TestAppSrcLifetimeState>(test_appsrc_lifetime_));
#endif
            generation_.fetch_add(1);
            changed = true;
        }
    }
    if (!accepted || !changed) {
        gst_object_unref(source);
    }
    if (previous) {
        gst_object_unref(previous);
    }
    if (changed) {
        latest_.clear();
        synchronizedLog(std::cout) << "[rtsp] media source bound generation=" << generation_.load() << '\n';
    }
    return accepted;
}

void RtspServer::onMediaConfigure(GstRTSPMediaFactory *factory, GstRTSPMedia *media) {
    // Keep media non-reusable so an errored packetizer pipeline is replaced by
    // a fresh one without racing mount/factory replacement. The hardware
    // encoder is independent and continues filling event pre-roll throughout.
    gst_rtsp_media_set_reusable(media, FALSE);
    gst_rtsp_media_set_stop_on_disconnect(media, FALSE);
    gst_rtsp_media_set_eos_shutdown(media, FALSE);
    gst_rtsp_media_set_suspend_mode(media, GST_RTSP_SUSPEND_MODE_NONE);

    const MediaHandlers handlers = connectMediaHandlers(media);
    GstRTSPMedia *previous_media = nullptr;
    GstAppSrc *previous_source = nullptr;
    MediaHandlers previous_handlers;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        const bool replacing_retired_media =
            current_media_ != nullptr &&
            (observed_status_ == GST_RTSP_MEDIA_STATUS_UNPREPARING ||
             observed_status_ == GST_RTSP_MEDIA_STATUS_ERROR);
        if (running_.load() && current_factory_ == factory &&
            (current_media_ == nullptr || replacing_retired_media)) {
            previous_media = current_media_;
            previous_source = appsrc_;
            previous_handlers = current_media_handlers_;
            current_media_ = static_cast<GstRTSPMedia *>(g_object_ref(media));
            current_media_handlers_ = handlers;
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            recovery_state_ = RecoveryState::Idle;
            recovery_generation_ = 0;
            recovery_token_ = 0;
            recovery_reason_.clear();
            recovery_started_us_ = 0;
            recovery_failure_reported_ = false;
            observed_status_ = GST_RTSP_MEDIA_STATUS_PREPARING;
            status_since_us_ = g_get_monotonic_time();
            accepted = true;
        }
    }
    if (!accepted) {
        disconnectMediaHandlers(media, handlers);
        synchronizedLog(std::cout) << "[rtsp] ignored concurrent media configuration\n";
        return;
    }
    disconnectMediaHandlers(previous_media, previous_handlers);
    if (previous_source) {
        gst_object_unref(previous_source);
    }
    if (previous_media) {
        g_object_unref(previous_media);
    }
    latest_.clear();
    if (!bindMediaSource(media)) {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ == media) {
            recovery_state_ = RecoveryState::Requested;
            recovery_generation_ = media_generation_;
            recovery_token_ = ++next_recovery_token_;
            recovery_reason_ = "media setup failure";
        }
        return;
    }
    synchronizedLog(std::cout) << "[rtsp] client media configured\n";
}

void RtspServer::onMediaPrepared(GstRTSPMedia *media) {
    if (bindMediaSource(media)) {
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (current_media_ == media) {
                consecutive_push_failures_ = 0;
                observed_status_ = GST_RTSP_MEDIA_STATUS_PREPARED;
                status_since_us_ = g_get_monotonic_time();
                current = true;
            }
        }
        if (current) {
            synchronizedLog(std::cout) << "[rtsp] client media prepared\n";
        }
    }
}

void RtspServer::onMediaUnprepared(GstRTSPMedia *media) {
    GstRTSPMedia *retired_media = nullptr;
    GstAppSrc *retired_source = nullptr;
    MediaHandlers retired_handlers;
    bool recovered_from_error = false;
    std::string recovery_reason;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ == media) {
            retired_media = current_media_;
            retired_source = appsrc_;
            retired_handlers = current_media_handlers_;
            recovered_from_error =
                recovery_state_ == RecoveryState::Running &&
                recovery_generation_ == media_generation_;
            recovery_reason = recovery_reason_;
            current_media_ = nullptr;
            current_media_handlers_ = {};
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            if (recovered_from_error) {
                recovery_media_unprepared_ = true;
            } else {
                recovery_state_ = RecoveryState::Idle;
                recovery_generation_ = 0;
                recovery_token_ = 0;
                recovery_reason_.clear();
                recovery_started_us_ = 0;
                recovery_failure_reported_ = false;
                recovery_media_unprepared_ = false;
                recovery_job_.reset();
            }
            observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
            status_since_us_ = g_get_monotonic_time();
        }
    }
    if (!retired_media) {
        synchronizedLog(std::cout) << "[rtsp] ignored stale media release\n";
        return;
    }
    latest_.clear();
    disconnectMediaHandlers(retired_media, retired_handlers);
    if (retired_source) {
        gst_object_unref(retired_source);
    }
    g_object_unref(retired_media);
    if (recovered_from_error) {
        synchronizedLog(std::cerr) << "[rtsp] recovery media unprepared; factory cache eviction confirmed: "
                  << recovery_reason << '\n';
    } else {
        synchronizedLog(std::cout) << "[rtsp] client media released; factory cache cleared\n";
    }
}

void RtspServer::onMediaTargetState(GstRTSPMedia *media, GstState state) {
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media || state != GST_STATE_NULL) {
            return;
        }
        observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARING;
        status_since_us_ = g_get_monotonic_time();
    }
    synchronizedLog(std::cout) << "[rtsp] media status=unpreparing\n";
}

void RtspServer::onMediaNewState(GstRTSPMedia *media, GstState state) {
    if (state == GST_STATE_READY || state == GST_STATE_PAUSED || state == GST_STATE_PLAYING) {
        bindMediaSource(media);
    }
    synchronizedLog(std::cout) << "[rtsp] media state=" << gst_element_state_get_name(state) << '\n';
}

gboolean RtspServer::onMediaHandleMessage(GstRTSPMedia *media, GstMessage *message) {
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR) {
        return FALSE;
    }

    bool first_request = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media) {
            return FALSE;
        }
        first_request = recovery_state_ == RecoveryState::Idle ||
                        recovery_generation_ != media_generation_;
        recovery_state_ = RecoveryState::Requested;
        recovery_generation_ = media_generation_;
        if (first_request) {
            recovery_token_ = ++next_recovery_token_;
        }
        recovery_reason_ = "media bus reported an error";
        observed_status_ = GST_RTSP_MEDIA_STATUS_ERROR;
        status_since_us_ = g_get_monotonic_time();
    }
    if (!first_request) {
        return FALSE;
    }

    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    metrics_.rtsp_errors.fetch_add(1);
    SynchronizedLogLine error_log(std::cerr);
    error_log << "[rtsp] media error: "
              << (error && error->message ? error->message : "unknown error");
    if (debug && *debug) {
        error_log << " (" << debug << ')';
    }
    error_log << '\n';
    g_clear_error(&error);
    g_free(debug);

    // Returning FALSE lets GstRTSPMedia's default run-last handler continue
    // processing the bus message after we have scheduled recovery.
    return FALSE;
}

gboolean RtspServer::onWatchdog() {
    if (!running_.load()) {
        return G_SOURCE_REMOVE;
    }
    std::shared_ptr<SessionCleanupState> cleanup_state;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        cleanup_state = session_cleanup_state_;
    }
    if (cleanup_state) {
        metrics_.rtsp_sessions_current.store(cleanup_state->current.load());
        metrics_.rtsp_sessions_cleaned.store(cleanup_state->cleaned.load());
    }

    const gint64 now = g_get_monotonic_time();
    finishRecoveryIfReady();
    std::uint64_t media_generation = 0;
    std::uint64_t recovery_generation = 0;
    std::string recovery_reason;
    GstRTSPMediaStatus status = GST_RTSP_MEDIA_STATUS_UNPREPARED;
    gint64 status_since_us = now;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_) {
            media_generation = media_generation_;
            recovery_generation = recovery_state_ == RecoveryState::Requested
                                      ? recovery_generation_
                                      : 0;
            recovery_reason = recovery_reason_;
            status = observed_status_;
            status_since_us = status_since_us_;
        }
    }

    if (recovery_generation != 0) {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (!test_recovery_pause_path_.empty() &&
            access(test_recovery_pause_path_.c_str(), F_OK) == 0) {
            if (!test_recovery_pause_reported_) {
                test_recovery_pause_reported_ = true;
                synchronizedLog(std::cerr) << "[rtsp] test hook holding pending recovery before watchdog\n";
            }
            return G_SOURCE_CONTINUE;
        }
        unsigned delay_ms = 0;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (running_.load() && recovery_state_ == RecoveryState::Requested &&
                recovery_generation_ == recovery_generation &&
                test_watchdog_recovery_delay_ms_ != 0) {
                delay_ms = test_watchdog_recovery_delay_ms_;
                test_watchdog_recovery_delay_ms_ = 0;
            }
        }
        if (delay_ms != 0) {
            synchronizedLog(std::cerr) << "[rtsp] test hook watchdog recovery entered; delaying "
                      << delay_ms << "ms\n";
            g_usleep(static_cast<gulong>(delay_ms) * 1000);
        }
#endif
        recoverMedia(recovery_generation,
                     recovery_reason.empty() ? "media recovery requested"
                                             : recovery_reason.c_str());
        return G_SOURCE_CONTINUE;
    }

    if (media_generation == 0) {
        return G_SOURCE_CONTINUE;
    }

    const gint64 status_age = now - status_since_us;
    if (status == GST_RTSP_MEDIA_STATUS_ERROR) {
        recoverMedia(media_generation, "media entered error state");
    } else if (status == GST_RTSP_MEDIA_STATUS_UNPREPARING &&
               status_age >= 3 * G_USEC_PER_SEC) {
        recoverMedia(media_generation, "media remained unpreparing for 3 seconds");
    } else if (status == GST_RTSP_MEDIA_STATUS_PREPARING &&
               status_age >= 10 * G_USEC_PER_SEC) {
        recoverMedia(media_generation, "media remained preparing for 10 seconds");
    }
    return G_SOURCE_CONTINUE;
}

bool RtspServer::recoverMedia(std::uint64_t expected_media_generation,
                              const char *reason) noexcept {
    std::uint64_t actual_media_generation = 0;
    GSource *listener_source = nullptr;
    bool begin_recovery = false;
    try {
        // Every throwing C++ allocation happens before any lifecycle field or
        // GObject ownership is changed. From the Running publication through
        // queue insertion, shared_ptr moves and GObject refs are non-throwing.
        auto job = std::make_shared<RecoveryJob>();
        std::string prepared_reason(reason ? reason : "media recovery requested");
        std::shared_ptr<RecoveryWorkerState> worker_state;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            actual_media_generation = current_media_ ? media_generation_ : 0;
            if (!running_.load() || !current_media_ ||
                media_generation_ != expected_media_generation) {
                if (recovery_state_ == RecoveryState::Requested &&
                    recovery_generation_ == expected_media_generation) {
                    recovery_state_ = RecoveryState::Idle;
                    recovery_generation_ = 0;
                    recovery_token_ = 0;
                    recovery_reason_.clear();
                }
            } else if (recovery_state_ == RecoveryState::Requested ||
                       recovery_state_ == RecoveryState::Idle) {
                worker_state = recovery_worker_state_;
                if (!worker_state) {
                    throw std::runtime_error("recovery worker is unavailable");
                }
                std::lock_guard<std::mutex> worker_lock(worker_state->mutex);
                if (worker_state->stopping || worker_state->pending) {
                    throw std::runtime_error("recovery worker cannot accept a job");
                }

                job->source = appsrc_;
                appsrc_ = nullptr;
                generation_.fetch_add(1);
                consecutive_push_failures_ = 0;
                recovery_state_ = RecoveryState::Running;
                recovery_generation_ = expected_media_generation;
                if (recovery_token_ == 0) {
                    recovery_token_ = ++next_recovery_token_;
                }
                recovery_reason_ = std::move(prepared_reason);
                recovery_started_us_ = g_get_monotonic_time();
                recovery_failure_reported_ = false;
                recovery_media_unprepared_ = false;
                job->token = recovery_token_;
                job->server = server_ ? GST_RTSP_SERVER(g_object_ref(server_)) : nullptr;
                job->pool = server_ ? gst_rtsp_server_get_session_pool(server_) : nullptr;
                job->factory = current_factory_
                                   ? GST_RTSP_MEDIA_FACTORY(g_object_ref(current_factory_))
                                   : nullptr;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
                job->teardown_delay_ms = test_teardown_delay_ms_;
#endif
                recovery_job_ = job;
                status_since_us_ = g_get_monotonic_time();
                listener_source = listener_source_;
                listener_source_ = nullptr;
                accepting_clients_ = false;
                worker_state->pending = std::move(job);
                begin_recovery = true;
            }
        }
        if (!begin_recovery) {
            if (actual_media_generation == expected_media_generation) {
                return false;
            }
            synchronizedLog(std::cout) << "[rtsp] ignored stale recovery request generation="
                      << expected_media_generation << " current="
                      << actual_media_generation << '\n';
            return false;
        }

        // Removing the listener and waking the pre-existing owner cannot throw
        // a C++ exception. The job already owns all teardown refs if stop wins
        // this boundary.
        latest_.clear();
        destroySource(listener_source);
        worker_state->cv.notify_one();
        synchronizedLog(std::cerr) << "[rtsp] recovery teardown started: "
                  << (reason ? reason : "media recovery requested") << '\n';
        return true;
    } catch (const std::exception &error) {
        bool report_failure = true;
        try {
            std::lock_guard<std::mutex> lock(source_mutex_);
            // Allocation/dispatch failures occur before Running is published.
            // Leave the current appsrc and listener intact and make the request
            // retryable instead of stranding a video-less generation.
            if (recovery_state_ == RecoveryState::Requested &&
                recovery_generation_ == expected_media_generation) {
                recovery_state_ = RecoveryState::Idle;
                recovery_generation_ = 0;
                recovery_token_ = 0;
                recovery_reason_.clear();
            } else if (recovery_state_ == RecoveryState::Running) {
                // This branch is defensive: the publish and queue operation is
                // non-throwing and atomic under the two owner mutexes.
                report_failure = !recovery_failure_reported_;
                recovery_failure_reported_ = true;
            }
        } catch (...) {
            // noexcept is the final C callback boundary. Atomic metrics below
            // still record the failure if even mutex acquisition is exhausted.
        }
        if (report_failure) {
            metrics_.rtsp_errors.fetch_add(1);
            metrics_.rtsp_recovery_failures.fetch_add(1);
        }
        synchronizedLog(std::cerr) << "[rtsp] recovery dispatch failed safely: " << error.what() << '\n';
        return false;
    } catch (...) {
        metrics_.rtsp_errors.fetch_add(1);
        metrics_.rtsp_recovery_failures.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] recovery dispatch failed safely: unknown exception\n";
        return false;
    }
}

void RtspServer::finishRecoveryIfReady() {
    std::string reason;
    std::uint64_t generation = 0;
    bool ready = false;
    bool report_timeout = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (running_.load() && recovery_state_ == RecoveryState::Running &&
            recovery_generation_ != 0 &&
            recovery_started_us_ != 0 && !recovery_failure_reported_ &&
            g_get_monotonic_time() - recovery_started_us_ >= 5 * G_USEC_PER_SEC) {
            recovery_failure_reported_ = true;
            generation = recovery_generation_;
            report_timeout = true;
        }
        ready = running_.load() && recovery_state_ == RecoveryState::Running &&
                recovery_generation_ != 0 &&
                recovery_media_unprepared_ && recovery_job_ &&
                recovery_job_->token == recovery_token_ &&
                recovery_job_->done.load(std::memory_order_acquire);
        if (ready) {
            reason = recovery_reason_;
            recovery_state_ = RecoveryState::Completed;
        }
    }
    if (report_timeout) {
        metrics_.rtsp_errors.fetch_add(1);
        metrics_.rtsp_recovery_failures.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] recovery failed: media did not reach UNPREPARED within 5 seconds"
                  << " generation=" << generation << '\n';
    }
    if (!ready) {
        return;
    }

    // G_CONNECT_AFTER guarantees the factory has evicted the non-reusable
    // media before recovery_media_unprepared_ becomes true. Only now is it
    // safe for a new request to construct fresh media from the stable factory.
    GSource *resumed_listener = createListenerSource();
    bool resumed = false;
    bool report_attach_failure = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (running_.load() && recovery_state_ == RecoveryState::Completed &&
            recovery_generation_ != 0 &&
            recovery_media_unprepared_ && recovery_job_ &&
            recovery_job_->token == recovery_token_ &&
            recovery_job_->done.load(std::memory_order_acquire) &&
            !accepting_clients_ && resumed_listener != nullptr) {
            listener_source_ = resumed_listener;
            accepting_clients_ = true;
            recovery_state_ = RecoveryState::Idle;
            recovery_generation_ = 0;
            recovery_token_ = 0;
            recovery_reason_.clear();
            recovery_started_us_ = 0;
            recovery_failure_reported_ = false;
            recovery_media_unprepared_ = false;
            recovery_job_.reset();
            resumed = true;
        } else if (running_.load() && resumed_listener == nullptr) {
            report_attach_failure = !recovery_failure_reported_;
            recovery_failure_reported_ = true;
        }
    }
    if (!resumed) {
        destroySource(resumed_listener);
    }
    if (resumed) {
        metrics_.rtsp_recoveries.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] recovery completed; media cache cleared and listener resumed: "
                  << reason << '\n';
    } else if (report_attach_failure) {
        metrics_.rtsp_errors.fetch_add(1);
        metrics_.rtsp_recovery_failures.fetch_add(1);
        synchronizedLog(std::cerr) << "[rtsp] recovery failed: listener did not resume\n";
    }
}

GstBuffer *RtspServer::makeBuffer(EncodedAccessUnitPtr unit,
                                  std::uint64_t frame_index) const {
    if (!unit || !unit->payload || unit->payload->empty()) {
        return nullptr;
    }
    auto *holder = new EncodedAccessUnitPtr(std::move(unit));
    GstBuffer *buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        const_cast<std::uint8_t *>((*holder)->payload->data()),
        (*holder)->payload->size(),
        0,
        (*holder)->payload->size(),
        holder,
        destroyEncodedUnit);
    if (!buffer) {
        delete holder;
        return nullptr;
    }
    const GstClockTime pts = frame_index * GST_SECOND / config_.fps;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = (*holder)->duration_ns != 0
                                      ? (*holder)->duration_ns
                                      : GST_SECOND / config_.fps;
    GST_BUFFER_OFFSET(buffer) = frame_index;
    GST_BUFFER_OFFSET_END(buffer) = frame_index + 1;
    GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_LIVE);
    if (!(*holder)->keyframe) {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    }
    return buffer;
}

void RtspServer::feederLoop() {
    std::uint64_t observed_generation = generation_.load();
    std::uint64_t frame_index = 0;
    std::uint64_t observed_encoder_generation = 0;
    bool awaiting_keyframe = true;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    bool force_initial_keyframe_drop = false;
#endif
    while (running_.load()) {
        EncodedAccessUnitPtr unit;
        if (!latest_.waitPop(unit)) {
            break;
        }

        GstAppSrcRef source;
        std::uint64_t generation = 0;
        std::uint64_t media_generation = 0;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            generation = generation_.load();
            media_generation = media_generation_;
            if (appsrc_) {
                source.reset(GST_APP_SRC(gst_object_ref(appsrc_)));
            }
        }
        if (!source) {
            metrics_.rtsp_dropped.fetch_add(1);
            continue;
        }
        if (generation != observed_generation ||
            observed_encoder_generation != unit->generation) {
            observed_generation = generation;
            observed_encoder_generation = unit->generation;
            frame_index = 0;
            awaiting_keyframe = true;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
            force_initial_keyframe_drop = test_force_initial_keyframe_drop_;
#endif
        }
        if (awaiting_keyframe) {
            bool independently_decodable = unit->independentlyDecodable();
#ifdef EGGVISION_ENABLE_TEST_HOOKS
            if (independently_decodable && force_initial_keyframe_drop) {
                force_initial_keyframe_drop = false;
                independently_decodable = false;
                synchronizedLog(std::cout) << "[rtsp] test hook discarding initial keyframe\n";
            }
#endif
            if (!independently_decodable) {
                metrics_.rtsp_dropped.fetch_add(1);
                continue;
            }
            awaiting_keyframe = false;
        }

        GstBuffer *buffer = makeBuffer(std::move(unit), frame_index++);
        if (!buffer) {
            metrics_.rtsp_errors.fetch_add(1);
            continue;
        }
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (!test_push_error_consumed_ && !test_push_error_trigger_.empty() &&
            access(test_push_error_trigger_.c_str(), F_OK) == 0) {
            test_push_error_consumed_ = true;
            test_push_errors_remaining_ = 3;
            synchronizedLog(std::cerr) << "[rtsp] test hook injecting three appsrc push errors\n";
        }
        GstFlowReturn flow = GST_FLOW_OK;
        if (test_push_errors_remaining_ > 0) {
            --test_push_errors_remaining_;
            gst_buffer_unref(buffer);
            flow = GST_FLOW_ERROR;
        } else {
            flow = gst_app_src_push_buffer(source.get(), buffer);
        }
#else
        const GstFlowReturn flow = gst_app_src_push_buffer(source.get(), buffer);
#endif
        if (flow == GST_FLOW_OK) {
            metrics_.rtsp_pushed.fetch_add(1);
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (current_media_ && media_generation_ == media_generation) {
                consecutive_push_failures_ = 0;
            }
        } else if (flow != GST_FLOW_FLUSHING) {
            metrics_.rtsp_errors.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(source_mutex_);
                if (current_media_ && media_generation_ == media_generation) {
                    ++consecutive_push_failures_;
                    if (consecutive_push_failures_ >= 3) {
                        if (recovery_state_ == RecoveryState::Idle ||
                            recovery_generation_ != media_generation) {
                            recovery_token_ = ++next_recovery_token_;
                        }
                        recovery_state_ = RecoveryState::Requested;
                        recovery_generation_ = media_generation;
                        recovery_reason_ = "repeated appsrc push failure";
                        if (consecutive_push_failures_ == 3) {
                            synchronizedLog(std::cerr) << "[rtsp] recovery request pending token="
                                      << recovery_token_ << " generation="
                                      << recovery_generation_ << '\n';
                        }
                    }
                }
            }
            synchronizedLog(std::cerr) << "[rtsp] appsrc push failed: " << gst_flow_get_name(flow) << '\n';
        } else {
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (current_media_ && media_generation_ == media_generation) {
                consecutive_push_failures_ = 0;
            }
        }
    }
}

void RtspServer::disableCallbacksAndWait() {
    std::unique_lock<std::mutex> lock(callback_state_->mutex);
    callback_state_->server = nullptr;
    callback_state_->cv.wait(lock, [this] { return callback_state_->active == 0; });
}

void RtspServer::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    stopLocked();
}

void RtspServer::stopLocked() {
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (!running_.load()) {
            return;
        }
        // Serialize the stopping publication with recoverMedia's final
        // running check. Whichever acquires this mutex first owns teardown.
        running_.store(false);
    }
    latest_.close();
    if (feeder_thread_.joinable()) {
        feeder_thread_.join();
    }
    GSource *watchdog_source = nullptr;
    GSource *listener_source = nullptr;
    std::shared_ptr<RecoveryJob> recovery_job;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        watchdog_source = watchdog_source_;
        watchdog_source_ = nullptr;
        listener_source = listener_source_;
        listener_source_ = nullptr;
        accepting_clients_ = false;
        // A Requested recovery has not taken ownership of any GStreamer
        // object. Cancel it and let this stop path perform the complete normal
        // teardown. A Running job must complete before this stop epoch can
        // release compressed-buffer owners or permit the next start epoch.
        if (recovery_state_ == RecoveryState::Running && recovery_job_ &&
            recovery_job_->token == recovery_token_ &&
            !recovery_job_->done.load(std::memory_order_acquire)) {
            recovery_job = recovery_job_;
        } else if (recovery_state_ == RecoveryState::Requested) {
            synchronizedLog(std::cerr) << "[rtsp] pending recovery cancelled by stop token="
                      << recovery_token_ << '\n';
        }
    }
    destroySource(watchdog_source);
    destroySource(listener_source);
    requestSessionCleanupStop();

    // A teardown already in flight must finish before its compressed buffers
    // and callbacks can be released. Keep the owner loop and Metrics alive
    // while waiting. Fifteen seconds
    // is an operational warning boundary, not a lifetime boundary: detaching
    // here would let callbacks outlive this server. If a driver really stalls
    // forever the process deliberately
    // remains in this explicit delayed-shutdown state and reports progress,
    // rather than returning from stop() with unsafe background references.
    if (recovery_job) {
        constexpr auto warning_boundary = std::chrono::seconds(15);
        constexpr auto warning_interval = std::chrono::seconds(5);
        const auto wait_started = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(recovery_job->mutex);
        bool completed = recovery_job->cv.wait_for(
            lock,
            warning_boundary,
            [&recovery_job] { return recovery_job->done.load(std::memory_order_acquire); });
        if (!completed) {
            bool report_failure = false;
            {
                std::lock_guard<std::mutex> source_lock(source_mutex_);
                report_failure = !recovery_failure_reported_;
                recovery_failure_reported_ = true;
            }
            if (report_failure) {
                metrics_.rtsp_errors.fetch_add(1);
                metrics_.rtsp_recovery_failures.fetch_add(1);
            }
            do {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - wait_started);
                synchronizedLog(std::cerr) << "[rtsp] shutdown delayed: recovery teardown token="
                          << recovery_job->token << " still running after "
                          << elapsed.count()
                          << " seconds; retaining owners and waiting safely\n";
                completed = recovery_job->cv.wait_for(
                    lock,
                    warning_interval,
                    [&recovery_job] {
                        return recovery_job->done.load(std::memory_order_acquire);
                    });
            } while (!completed);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wait_started);
            synchronizedLog(std::cerr) << "[rtsp] delayed recovery teardown completed token="
                      << recovery_job->token << " elapsed_ms=" << elapsed.count()
                      << "; shutdown continuing\n";
        }
    }

    // No new job can be published after running_=false. Always join the
    // persistent owner before callbacks, media, Metrics or a subsequent start
    // epoch can proceed.
    requestRecoveryWorkerStop();

    if (server_ && client_connected_handler_ != 0 &&
        g_signal_handler_is_connected(server_, client_connected_handler_)) {
        g_signal_handler_disconnect(server_, client_connected_handler_);
    }
    client_connected_handler_ = 0;

    // Media signals run on GstRTSPThread contexts, not necessarily loop_thread_.
    // Make every callback drop its independently-owned state reference, then
    // wait for callbacks that already captured this server to finish before
    // releasing any GStreamer object or the server itself.
    disableCallbacksAndWait();

    GstAppSrc *source = nullptr;
    GstRTSPMedia *media = nullptr;
    MediaHandlers media_handlers;
    GstRTSPMediaFactory *factory = nullptr;
    gulong factory_handler = 0;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        source = appsrc_;
        media = current_media_;
        media_handlers = current_media_handlers_;
        factory = current_factory_;
        factory_handler = current_factory_handler_;
        appsrc_ = nullptr;
        current_media_ = nullptr;
        current_media_handlers_ = {};
        current_factory_ = nullptr;
        current_factory_handler_ = 0;
        generation_.fetch_add(1);
        ++media_generation_;
        consecutive_push_failures_ = 0;
        recovery_state_ = RecoveryState::Idle;
        recovery_generation_ = 0;
        recovery_token_ = 0;
        recovery_reason_.clear();
        recovery_started_us_ = 0;
        recovery_failure_reported_ = false;
        recovery_media_unprepared_ = false;
        recovery_job_.reset();
        observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
        status_since_us_ = g_get_monotonic_time();
    }

    disconnectMediaHandlers(media, media_handlers);
    if (factory && factory_handler != 0 &&
        g_signal_handler_is_connected(factory, factory_handler)) {
        g_signal_handler_disconnect(factory, factory_handler);
    }
    if (mounts_) {
        gst_rtsp_mount_points_remove_factory(mounts_, config_.rtsp_mount.c_str());
    }
    if (server_) {
        GList *clients = gst_rtsp_server_client_filter(server_, RtspServer::closeClient, nullptr);
        g_list_free_full(clients, g_object_unref);
    }

    if (source) {
        gst_app_src_end_of_stream(source);
    }
    if (media) {
        gst_rtsp_media_set_pipeline_state(media, GST_STATE_NULL);
        gst_rtsp_media_unprepare(media);
    }
    if (source) {
        gst_object_unref(source);
    }
    if (loop_) {
        g_main_loop_quit(loop_);
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    if (media) {
        g_object_unref(media);
    }
    if (factory) {
        g_object_unref(factory);
    }
    if (mounts_) {
        g_object_unref(mounts_);
        mounts_ = nullptr;
    }
    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
    synchronizedLog(std::cout) << "[rtsp] stopped\n";
}

std::string RtspServer::url(const std::string &host) const {
    return "rtsp://" + host + ':' + config_.rtsp_port + config_.rtsp_mount;
}

#ifdef EGGVISION_ENABLE_TEST_HOOKS
bool RtspServer::recoveryRunningForTest() const {
    std::lock_guard<std::mutex> lock(source_mutex_);
    return recovery_state_ == RecoveryState::Running && recovery_job_ &&
           !recovery_job_->done.load(std::memory_order_acquire);
}
#endif

}  // namespace eggvision
