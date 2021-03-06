#pragma once

namespace stream_client {
namespace connector {

template <typename Connector>
template <typename... ArgN>
base_connection_pool<Connector>::base_connection_pool(std::size_t size, ArgN&&... argn)
    : connector_(std::forward<ArgN>(argn)...)
    , pool_size_(size)
    , watch_pool(true)
{
    pool_watcher_ = std::thread([this]() { this->watch_pool_routine(); });
}

template <typename Connector>
base_connection_pool<Connector>::~base_connection_pool()
{
    watch_pool = false;
    if (pool_watcher_.joinable()) {
        pool_watcher_.join();
    }
}

template <typename Connector>
std::unique_ptr<typename base_connection_pool<Connector>::stream_type>
base_connection_pool<Connector>::get_session(boost::system::error_code& ec, const time_point_type& deadline)
{
    std::unique_lock<std::timed_mutex> pool_lk(pool_mutex_, std::defer_lock);
    if (!pool_lk.try_lock_until(deadline)) {
        // failed to lock pool_mutex_
        ec = boost::asio::error::timed_out;
        return nullptr;
    }
    if (sesson_pool_.empty() && !pool_cv_.wait_until(pool_lk, deadline, [this] { return !sesson_pool_.empty(); })) {
        // session pool is still empty
        ec = boost::asio::error::timed_out;
        return nullptr;
    }

    std::unique_ptr<stream_type> session = std::move(sesson_pool_.front());
    sesson_pool_.pop_front();
    return session;
}

template <typename Connector>
void base_connection_pool<Connector>::return_session(std::unique_ptr<stream_type>&& session)
{
    if (!session || !session->next_layer().is_open()) {
        return;
    }

    std::unique_lock<std::timed_mutex> pool_lk(pool_mutex_, std::defer_lock);
    if (!pool_lk.try_lock_for(std::chrono::milliseconds(1))) {
        // if we failed to return session in 1ms it's easier to establish new one
        return;
    }

    sesson_pool_.emplace_back(std::move(session));
    pool_lk.unlock();
    pool_cv_.notify_all();
}

template <typename Connector>
bool base_connection_pool<Connector>::is_connected(boost::system::error_code& ec, const time_point_type& deadline) const
{
    std::unique_lock<std::timed_mutex> pool_lk(pool_mutex_, std::defer_lock);
    if (!pool_lk.try_lock_until(deadline)) {
        // failed to lock pool_mutex_
        ec = boost::asio::error::timed_out;
        return false;
    }
    if (sesson_pool_.empty() && !pool_cv_.wait_until(pool_lk, deadline, [this] { return !sesson_pool_.empty(); })) {
        // session pool is still empty
        return false;
    }
    return true;
}

template <typename Connector>
void base_connection_pool<Connector>::watch_pool_routine()
{
    static const auto lock_timeout = std::chrono::milliseconds(100);

    while (watch_pool) {
        std::unique_lock<std::timed_mutex> pool_lk(pool_mutex_, std::defer_lock);
        if (!pool_lk.try_lock_for(lock_timeout)) {
            continue;
        }
        int vacant_places = pool_size_ - sesson_pool_.size();
        if (vacant_places < 0) {
            vacant_places = 0;
        }

        // at this point we own pool_mutex_, but we want to get new sessions simultaneously;
        // that's why new mutex to sync adding threads
        std::mutex pool_add;
        bool added = false;
        auto add_session = [&pool_add, &added, &connector = this->connector_, &pool = this->sesson_pool_]() {
            try {
                // getting new session is time consuming operation
                auto new_session = connector.new_session();
                // ensure only single session added at time
                std::unique_lock<std::mutex> add_lk(pool_add);
                pool.emplace_back(std::move(new_session));
                added = true;
            } catch (const boost::system::system_error& e) {
                // NOOP
            }
        };

        std::list<std::thread> adders;
        for (int i = 0; i < vacant_places; ++i) {
            adders.emplace_back(add_session);
        }
        for (auto& a : adders) {
            a.join();
        }

        pool_lk.unlock();
        if (added) {
            pool_cv_.notify_all();
        } else {
            // stop cpu spooling if no session has been added
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace connector
} // namespace stream_client
