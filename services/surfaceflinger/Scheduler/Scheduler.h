/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <ui/DisplayStatInfo.h>
#include <ui/GraphicTypes.h>

#include "DispSync.h"
#include "EventControlThread.h"
#include "EventThread.h"
#include "IdleTimer.h"
#include "InjectVSyncSource.h"
#include "LayerHistory.h"
#include "RefreshRateConfigs.h"
#include "SchedulerUtils.h"

namespace android {

class EventControlThread;

class Scheduler {
public:
    // Enum to keep track of whether we trigger event to notify choreographer of config changes.
    enum class ConfigEvent { None, Changed };

    // logical or operator with the semantics of at least one of the events is Changed
    friend ConfigEvent operator|(const ConfigEvent& first, const ConfigEvent& second) {
        if (first == ConfigEvent::Changed) return ConfigEvent::Changed;
        if (second == ConfigEvent::Changed) return ConfigEvent::Changed;
        return ConfigEvent::None;
    }

    using RefreshRateType = scheduler::RefreshRateConfigs::RefreshRateType;
    using ChangeRefreshRateCallback = std::function<void(RefreshRateType, ConfigEvent)>;
    using GetVsyncPeriod = std::function<nsecs_t()>;

    // Enum to indicate whether to start the transaction early, or at vsync time.
    enum class TransactionStart { EARLY, NORMAL };

    /* The scheduler handle is a BBinder object passed to the client from which we can extract
     * an ID for subsequent operations.
     */
    class ConnectionHandle : public BBinder {
    public:
        ConnectionHandle(int64_t id) : id(id) {}

        ~ConnectionHandle() = default;

        const int64_t id;
    };

    class Connection {
    public:
        Connection(sp<ConnectionHandle> handle, sp<EventThreadConnection> eventConnection,
                   std::unique_ptr<EventThread> eventThread)
              : handle(handle), eventConnection(eventConnection), thread(std::move(eventThread)) {}

        ~Connection() = default;

        sp<ConnectionHandle> handle;
        sp<EventThreadConnection> eventConnection;
        const std::unique_ptr<EventThread> thread;
    };

    // Stores per-display state about VSYNC.
    struct VsyncState {
        explicit VsyncState(Scheduler& scheduler) : scheduler(scheduler) {}

        void resync(const GetVsyncPeriod&);

        Scheduler& scheduler;
        std::atomic<nsecs_t> lastResyncTime = 0;
    };

    explicit Scheduler(impl::EventControlThread::SetVSyncEnabledFunction function);

    virtual ~Scheduler();

    /** Creates an EventThread connection. */
    sp<ConnectionHandle> createConnection(const char* connectionName, int64_t phaseOffsetNs,
                                          ResyncCallback,
                                          impl::EventThread::InterceptVSyncsCallback);

    sp<IDisplayEventConnection> createDisplayEventConnection(const sp<ConnectionHandle>& handle,
                                                             ResyncCallback);

    // Getter methods.
    EventThread* getEventThread(const sp<ConnectionHandle>& handle);

    // Provides access to the DispSync object for the primary display.
    void withPrimaryDispSync(std::function<void(DispSync&)> const& fn);

    sp<EventThreadConnection> getEventConnection(const sp<ConnectionHandle>& handle);

    // Should be called when receiving a hotplug event.
    void hotplugReceived(const sp<ConnectionHandle>& handle, PhysicalDisplayId displayId,
                         bool connected);

    // Should be called after the screen is turned on.
    void onScreenAcquired(const sp<ConnectionHandle>& handle);

    // Should be called before the screen is turned off.
    void onScreenReleased(const sp<ConnectionHandle>& handle);

    // Should be called when display config changed
    void onConfigChanged(const sp<ConnectionHandle>& handle, PhysicalDisplayId displayId,
                         int32_t configId);

    // Should be called when dumpsys command is received.
    void dump(const sp<ConnectionHandle>& handle, std::string& result) const;

    // Offers ability to modify phase offset in the event thread.
    void setPhaseOffset(const sp<ConnectionHandle>& handle, nsecs_t phaseOffset);

    // pause/resume vsync callback generation to avoid sending vsync callbacks during config switch
    void pauseVsyncCallback(const sp<ConnectionHandle>& handle, bool pause);

    void getDisplayStatInfo(DisplayStatInfo* stats);

    void enableHardwareVsync();
    void disableHardwareVsync(bool makeUnavailable);
    void resyncToHardwareVsync(bool makeAvailable, nsecs_t period);
    // Creates a callback for resyncing.
    ResyncCallback makeResyncCallback(GetVsyncPeriod&& getVsyncPeriod);
    void setRefreshSkipCount(int count);
    void addResyncSample(const nsecs_t timestamp);
    void addPresentFence(const std::shared_ptr<FenceTime>& fenceTime);
    void setIgnorePresentFences(bool ignore);
    nsecs_t expectedPresentTime();
    // apiId indicates the API (NATIVE_WINDOW_API_xxx) that queues the buffer.
    // TODO(b/123956502): Remove this call with V1 go/content-fps-detection-in-scheduler.
    void addNativeWindowApi(int apiId);
    // Updates FPS based on the most occured request for Native Window API.
    void updateFpsBasedOnNativeWindowApi();
    // Callback that gets invoked when Scheduler wants to change the refresh rate.
    void setChangeRefreshRateCallback(const ChangeRefreshRateCallback& changeRefreshRateCallback);

    // Returns whether idle timer is enabled or not
    bool isIdleTimerEnabled() { return mSetIdleTimerMs > 0; }
    // Returns relevant information about Scheduler for dumpsys purposes.
    std::string doDump();

    // calls DispSync::dump() on primary disp sync
    void dumpPrimaryDispSync(std::string& result) const;

protected:
    virtual std::unique_ptr<EventThread> makeEventThread(
            const char* connectionName, DispSync* dispSync, int64_t phaseOffsetNs,
            impl::EventThread::InterceptVSyncsCallback interceptCallback);

private:
    friend class TestableScheduler;

    // In order to make sure that the features don't override themselves, we need a state machine
    // to keep track which feature requested the config change.
    enum class MediaFeatureState { MEDIA_PLAYING, MEDIA_OFF };
    enum class IdleTimerState { EXPIRED, RESET };

    // Creates a connection on the given EventThread and forwards the given callbacks.
    sp<EventThreadConnection> createConnectionInternal(EventThread*, ResyncCallback&&);

    nsecs_t calculateAverage() const;
    void updateFrameSkipping(const int64_t skipCount);
    // Function that resets the idle timer.
    void resetIdleTimer();
    // Function that is called when the timer resets.
    void resetTimerCallback();
    // Function that is called when the timer expires.
    void expiredTimerCallback();
    // Sets vsync period.
    void setVsyncPeriod(const nsecs_t period);
    // Media feature's function to change the refresh rate.
    void mediaChangeRefreshRate(MediaFeatureState mediaFeatureState);
    // Idle timer feature's function to change the refresh rate.
    void timerChangeRefreshRate(IdleTimerState idleTimerState);
    // Acquires a lock and calls the ChangeRefreshRateCallback() with given parameters.
    void changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent);

    // If fences from sync Framework are supported.
    const bool mHasSyncFramework;

    // The offset in nanoseconds to use, when DispSync timestamps present fence
    // signaling time.
    nsecs_t mDispSyncPresentTimeOffset;

    // Each connection has it's own ID. This variable keeps track of the count.
    static std::atomic<int64_t> sNextId;

    // Connections are stored in a map <connection ID, connection> for easy retrieval.
    std::unordered_map<int64_t, std::unique_ptr<Connection>> mConnections;

    std::mutex mHWVsyncLock;
    bool mPrimaryHWVsyncEnabled GUARDED_BY(mHWVsyncLock);
    bool mHWVsyncAvailable GUARDED_BY(mHWVsyncLock);
    const std::shared_ptr<VsyncState> mPrimaryVsyncState{std::make_shared<VsyncState>(*this)};

    std::unique_ptr<DispSync> mPrimaryDispSync;
    std::unique_ptr<EventControlThread> mEventControlThread;

    // TODO(b/113612090): The following set of variables needs to be revised. For now, this is
    // a proof of concept. We turn on frame skipping if the difference between the timestamps
    // is between 32 and 34ms. We expect this currently for 30fps videos, so we render them at 30Hz.
    nsecs_t mPreviousFrameTimestamp = 0;
    // Keeping track of whether we are skipping the refresh count. If we want to
    // simulate 30Hz rendering, we skip every other frame, and this variable is set
    // to 1.
    int64_t mSkipCount = 0;
    std::array<int64_t, scheduler::ARRAY_SIZE> mTimeDifferences{};
    size_t mCounter = 0;

    // The following few fields follow native window api bits that come with buffers. If there are
    // more buffers with NATIVE_WINDOW_API_MEDIA we render at 60Hz, otherwise we render at 90Hz.
    // There is not dependency on timestamp for V0.
    // TODO(b/123956502): Remove this when more robust logic for content fps detection is developed.
    std::mutex mWindowApiHistoryLock;
    std::array<int, scheduler::ARRAY_SIZE> mWindowApiHistory GUARDED_BY(mWindowApiHistoryLock);
    int64_t mApiHistoryCounter = 0;

    // Timer that records time between requests for next vsync. If the time is higher than a given
    // interval, a callback is fired. Set this variable to >0 to use this feature.
    int64_t mSetIdleTimerMs = 0;
    std::unique_ptr<scheduler::IdleTimer> mIdleTimer;

    std::mutex mCallbackLock;
    ChangeRefreshRateCallback mChangeRefreshRateCallback GUARDED_BY(mCallbackLock);

    // In order to make sure that the features don't override themselves, we need a state machine
    // to keep track which feature requested the config change.
    std::mutex mFeatureStateLock;
    MediaFeatureState mCurrentMediaFeatureState GUARDED_BY(mFeatureStateLock) =
            MediaFeatureState::MEDIA_OFF;
    IdleTimerState mCurrentIdleTimerState GUARDED_BY(mFeatureStateLock) = IdleTimerState::RESET;
};

} // namespace android