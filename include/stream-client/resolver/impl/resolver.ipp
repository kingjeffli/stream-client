#pragma once

namespace stream_client {
namespace resolver {

template <typename Protocol>
base_resolver<Protocol>::base_resolver(std::string host, std::string port, time_duration_type resolve_timeout,
                                       ip_family protocol, resolve_flags_type resolve_flags)
    : ::stream_client::detail::steady_timed_base()
    , resolver_(get_io_service())
    , resolve_timeout_(std::move(resolve_timeout))
{
    switch (protocol) {
    case ip_family::ipv4:
        query_ = std::make_unique<query_type>(Protocol::v4(), host, port, std::move(resolve_flags));
        break;
    case ip_family::ipv6:
        query_ = std::make_unique<query_type>(Protocol::v6(), host, port, std::move(resolve_flags));
        break;
    case ip_family::any:
        query_ = std::make_unique<query_type>(host, port, std::move(resolve_flags));
        break;
    }
}

template <typename Protocol>
template <typename Time>
typename base_resolver<Protocol>::iterator_type base_resolver<Protocol>::resolve(boost::system::error_code& ec,
                                                                                 const Time& timeout_or_deadline)
{
    iterator_type endpoints;
    ec = boost::asio::error::would_block;

    async_resolve(
        [&, this](const boost::system::error_code& error, iterator_type iterator) {
            endpoints = std::move(iterator);
            if (this->deadline_fired_) {
                ec = boost::asio::error::timed_out;
            } else {
                ec = error;
            }
        },
        timeout_or_deadline);

    while (ec == boost::asio::error::would_block) {
        get_io_service().run_one();
    };
    return endpoints;
}

template <typename Protocol>
void base_resolver<Protocol>::deadline_actor()
{
    resolver_.cancel(); // canceled operation error code will be boost::asio::error::operation_aborted
}

} // namespace resolver
} // namespace stream_client
