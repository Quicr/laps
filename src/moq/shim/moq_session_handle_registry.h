#pragma once

#include "types.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace laps::moq::shim {

    /**
     * @brief P0: Assign laps/quicr-style ConnectionHandle values to backend session objects.
     *
     * Thread-safe minimal registry. Invalidates handles when sessions expire (erase on remove).
     */
    template<typename Session>
    class MoqSessionHandleRegistry
    {
      public:
        MoqSessionHandleRegistry()
          : next_handle_(1)
        {
        }

        /** Returns 0 on failure (null session). */
        ConnectionHandle RegisterSession(std::shared_ptr<Session> session)
        {
            if (!session) {
                return 0;
            }
            std::lock_guard<std::mutex> registry_lock(mutex_);
            const ConnectionHandle handle = next_handle_++;
            by_handle_.emplace(handle, session);
            by_session_.emplace(session.get(), handle);
            return handle;
        }

        void RemoveByHandle(ConnectionHandle handle)
        {
            std::lock_guard<std::mutex> registry_lock(mutex_);
            auto handle_it = by_handle_.find(handle);
            if (handle_it == by_handle_.end()) {
                return;
            }
            if (auto locked_session = handle_it->second.lock()) {
                by_session_.erase(locked_session.get());
            }
            by_handle_.erase(handle_it);
        }

        std::shared_ptr<Session> LockSession(ConnectionHandle handle) const
        {
            std::lock_guard<std::mutex> registry_lock(mutex_);
            auto handle_it = by_handle_.find(handle);
            if (handle_it == by_handle_.end()) {
                return nullptr;
            }
            return handle_it->second.lock();
        }

        /**
         * @brief Resolve handle for a live session. Pass the same `shared_ptr` identity as at
         *        registration (or any `shared_ptr` with the same raw address).
         */
        bool TryFindHandle(const std::shared_ptr<Session>& session, ConnectionHandle& out_handle) const
        {
            if (!session) {
                return false;
            }
            std::lock_guard<std::mutex> registry_lock(mutex_);
            auto session_it = by_session_.find(session.get());
            if (session_it == by_session_.end()) {
                return false;
            }
            out_handle = session_it->second;
            return true;
        }

      private:
        mutable std::mutex mutex_;
        ConnectionHandle next_handle_;
        std::unordered_map<ConnectionHandle, std::weak_ptr<Session>> by_handle_;
        /** Keys are stable for the session lifetime (set at RegisterSession from `shared_ptr::get()`). */
        std::unordered_map<const Session*, ConnectionHandle> by_session_;
    };

} // namespace laps::moq::shim
