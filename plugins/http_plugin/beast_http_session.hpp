#pragma once

#include "common.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

extern fc::logger logger;

namespace eosio { 

    using std::chrono::steady_clock;

    typedef tcp::socket tcp_socket_t;

    using boost::asio::local::stream_protocol;
    
#if BOOST_VERSION < 107300
    using local_stream = beast::basic_stream<
    stream_protocol,
    asio::executor,
    beast::unlimited_rate_policy>;
#else      
using local_stream = beast::basic_stream<
    stream_protocol,
    asio::any_io_executor,
    beast::unlimited_rate_policy>;    
#endif    

    //------------------------------------------------------------------------------
    // Report a failure
    void fail(beast::error_code ec, char const* what)
    {
        // ssl::error::stream_truncated, also known as an SSL "short read",
        // indicates the peer closed the connection without performing the
        // required closing handshake (for example, Google does this to
        // improve performance). Generally this can be a security issue,
        // but if your communication protocol is self-terminated (as
        // it is with both HTTP and WebSocket) then you may simply
        // ignore the lack of close_notify.
        //
        // https://github.com/boostorg/beast/issues/38
        //
        // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
        //
        // When a short read would cut off the end of an HTTP message,
        // Beast returns the error beast::http::error::partial_message.
        // Therefore, if we see a short read here, it has occurred
        // after the message has been completed, so it is safe to ignore it.

        if(ec == asio::ssl::error::stream_truncated)
            return;

        fc_elog(logger, "${w}: ${m}", ("w", what)("m", ec.message()));
    }

    // this needs to be declared outside of the beast_http_session, because it doesn't work for 
    // UNIX socket session, since it lacks address() function in endpoint
    //  Since this is a virtual function, std::enable_if_t/SFNIAE doesn't work 
    template<class T>
    bool allow_host(const http::request<http::string_body>& req, T& session, std::shared_ptr<http_plugin_state> plugin_state) {
        auto is_conn_secure = session.is_secure();

        auto& lowest_layer = beast::get_lowest_layer(session.stream());

        auto local_endpoint = lowest_layer.socket().local_endpoint();
        auto local_socket_host_port = local_endpoint.address().to_string() 
                + ":" + std::to_string(local_endpoint.port());
        const auto& host_str = req["Host"].to_string();
        if (host_str.empty() 
            || !host_is_valid(*plugin_state, 
                                host_str, 
                                local_socket_host_port, 
                                is_conn_secure)) 
        {
            return false;
        }

        return true;
    }

    // Handle HTTP conneciton using boost::beast for TCP communication
    // This uses the Curiously Recurring Template Pattern so that
    // the same code works with both SSL streams and regular sockets.
    template<class Derived> 
    class beast_http_session :  public detail::abstract_conn
    {
        protected:
            beast::flat_buffer buffer_;
            beast::error_code ec_;
            std::string errStr_;

            // time points for timeout measurement and perf metrics 
            steady_clock::time_point session_begin_, read_begin_, handle_begin_, write_begin_;
            uint64_t read_time_us_, handle_time_us_, write_time_us_;

            // HTTP parser object
            http::request_parser<http::string_body> req_parser_;

            // HTTP response object
            http::response<http::string_body> res_;

            std::shared_ptr<http_plugin_state> plugin_state_;
            
            template<
                class Body, class Allocator>
            void
            handle_request(
                http::request<Body, http::basic_fields<Allocator>>&& req)
            {
                auto &res = res_;

                res.version(req.version());
                res.set(http::field::content_type, "application/json");
                res.keep_alive(req.keep_alive());
                res.set(http::field::server, BOOST_BEAST_VERSION_STRING);

                // Returns a bad request response
                auto const bad_request =
                [](beast::string_view why, detail::abstract_conn& conn)
                {
                    conn.send_response(std::string(why), 
                                    static_cast<int>(http::status::bad_request));
                };

                // Returns a not found response
                auto const not_found =
                [](beast::string_view target, detail::abstract_conn& conn)
                {
                    conn.send_response("The resource '" + std::string(target) + "' was not found.",
                                static_cast<int>(http::status::not_found)); 
                };
                
                // Request path must be absolute and not contain "..".
                if( req.target().empty() ||
                    req.target()[0] != '/' ||
                    req.target().find("..") != beast::string_view::npos)
                    return bad_request("Illegal request-target", *this);

                try {
                    if(!derived().allow_host(req))
                        return;

                    if( !plugin_state_->access_control_allow_origin.empty()) {
                        res.set( "Access-Control-Allow-Origin", plugin_state_->access_control_allow_origin );
                    }
                    if( !plugin_state_->access_control_allow_headers.empty()) {
                        res.set( "Access-Control-Allow-Headers", plugin_state_->access_control_allow_headers );
                    }
                    if( !plugin_state_->access_control_max_age.empty()) {
                        res.set( "Access-Control-Max-Age", plugin_state_->access_control_max_age );
                    }
                    if( plugin_state_->access_control_allow_credentials ) {
                        res.set( "Access-Control-Allow-Credentials", "true" );
                    }

                    // Respond to options request
                    if(req.method() == http::verb::options)
                    {
                        send_response("", static_cast<int>(http::status::ok));
                        return;
                    }

                    // verfiy bytes in flight/requests in flight
                    if( !verify_max_bytes_in_flight() ) return;

                    std::string resource = std::string(req.target());
                    // look for the URL handler to handle this reosouce
                    auto handler_itr = plugin_state_->url_handlers.find( resource );
                    if( handler_itr != plugin_state_->url_handlers.end()) {
                        std::string body = req.body();
                        auto resp_h = make_http_response_handler(derived().stream().get_executor(), *plugin_state_, derived().shared_from_this());
                        handler_itr->second( derived().shared_from_this(), 
                                            std::move( resource ), 
                                            std::move( body ), 
                                            resp_h );
                    } else {
                        fc_dlog( logger, "404 - not found: ${ep}", ("ep", resource) );
                        not_found(resource, *this);                    
                    }
                } catch( ... ) {
                    handle_exception();
                }           
            }

            void report_429_error(const std::string & what) {                
                send_response(std::string(what), 
                            static_cast<int>(http::status::too_many_requests));                
            }

        public:
            virtual bool verify_max_bytes_in_flight() override {
                auto bytes_in_flight_size = plugin_state_->bytes_in_flight.load();
                if( bytes_in_flight_size > plugin_state_->max_bytes_in_flight ) {
                    fc_dlog( logger, "429 - too many bytes in flight: ${bytes}", ("bytes", bytes_in_flight_size) );
                    string what = "Too many bytes in flight: " + std::to_string( bytes_in_flight_size ) + ". Try again later.";;
                    report_429_error(what);
                    return false;
                }
                return true;
            }

            virtual bool verify_max_requests_in_flight() override {
                if (plugin_state_->max_requests_in_flight < 0)
                    return true;

                auto requests_in_flight_num = plugin_state_->requests_in_flight.load();
                if( requests_in_flight_num > plugin_state_->max_requests_in_flight ) {
                    fc_dlog( logger, "429 - too many requests in flight: ${requests}", ("requests", requests_in_flight_num) );
                    string what = "Too many requests in flight: " + std::to_string( requests_in_flight_num ) + ". Try again later.";
                    report_429_error(what);
                    return false;
                }
                return true;
            }           

            // Access the derived class, this is part of
            // the Curiously Recurring Template Pattern idiom.
            Derived& derived()
            {
                return static_cast<Derived&>(*this);
            }
            
        public: 
            // shared_from_this() requires default constuctor
            beast_http_session() = default;
      
            // Take ownership of the buffer
            beast_http_session(
                std::shared_ptr<http_plugin_state> plugin_state) 
                : plugin_state_(plugin_state)
            {  
                plugin_state_->requests_in_flight += 1;
                req_parser_.body_limit(plugin_state_->max_body_size);

                session_begin_ = steady_clock::now();
                read_time_us_ = handle_time_us_ = write_time_us_ = 0;
            }

            virtual ~beast_http_session() {
                plugin_state_->requests_in_flight -= 1;
#if PRINT_PERF_METRICS                
                auto session_time = steady_clock::now() - session_begin_;
                auto session_time_us = std::chrono::duration_cast<std::chrono::microseconds>(session_time).count();           
                fc_dlog(logger, "session time    ${t}", ("t", session_time_us));                            
                fc_dlog(logger, "        read    ${t}", ("t", read_time_us_));
                fc_dlog(logger, "        handle  ${t}", ("t", handle_time_us_));                                            
                fc_dlog(logger, "        write   ${t}", ("t", write_time_us_));                            
#endif
            }    

            void do_read()
            {
                read_begin_ = steady_clock::now();

                // Read a request
                auto self = derived().shared_from_this();
                http::async_read(
                    derived().stream(),
                    buffer_,
                    req_parser_,
                    [self](beast::error_code ec, std::size_t bytes_transferred) { 
                        self->on_read(ec, bytes_transferred);
                    }
                );                
            }

            void on_read(beast::error_code ec,
                         std::size_t bytes_transferred)
            {
                boost::ignore_unused(bytes_transferred);

                // This means they closed the connection
                if(ec == http::error::end_of_stream)
                    return derived().do_eof();

                if(ec && ec != asio::ssl::error::stream_truncated)
                    return fail(ec, "read");

                auto req = req_parser_.get();

                handle_begin_ = steady_clock::now();
                auto dt = handle_begin_ - read_begin_;
                read_time_us_ += std::chrono::duration_cast<std::chrono::microseconds>(dt).count();

                // Send the response
                handle_request(std::move(req));
            }

            void on_write(beast::error_code ec,
                          std::size_t bytes_transferred, 
                          bool close)
            {
                boost::ignore_unused(bytes_transferred);

                if(ec) {
                    ec_ = ec;
                    handle_exception();
                    return fail(ec, "write");
                }

                auto dt = steady_clock::now() - write_begin_;
                write_time_us_ += std::chrono::duration_cast<std::chrono::microseconds>(dt).count();

                if(close)
                {
                    // This means we should close the connection, usually because
                    // the response indicated the "Connection: close" semantic.
                    return derived().do_eof();
                }

                // Read another request
                do_read();
            }           

            virtual void handle_exception() override {
                auto errStr = errStr_;
                if(errStr.size() < 1) { 
                    errStr = std::to_string(ec_.value());
                    fc_elog( logger, "beast_http_session_exception: beast error code ${ec}", ("ec", errStr));
                } else {
                    fc_elog( logger, "beast_websession_exception: error ${e}", ("e", errStr));
                }

                res_.set(http::field::content_type, "text/plain");
                res_.keep_alive(false);
                res_.set(http::field::server, BOOST_BEAST_VERSION_STRING);

                std::string err = "Internal server error";
                http::status stat = http::status::internal_server_error;
                send_response(errStr, static_cast<int>(stat));
            }

            virtual void send_response(std::optional<std::string> body, int code) override {
                // Determine if we should close the connection after
                bool close = !(plugin_state_->keep_alive) || res_.need_eof();

                write_begin_ = steady_clock::now();
                auto dt = write_begin_ - handle_begin_;
                handle_time_us_ += std::chrono::duration_cast<std::chrono::microseconds>(dt).count();
                
                res_.result(code);
                if(body.has_value())
                    res_.body() = *body;        

                res_.prepare_payload();

                // Write the response
                auto self = derived().shared_from_this();
                http::async_write(
                    derived().stream(),
                    res_,
                    [self, close](beast::error_code ec, std::size_t bytes_transferred) {
                        self->on_write(ec, bytes_transferred, close);
                    }
                );
            }

            void run_session() {
                // wrap entiere session around try/catch to prevent non-zero exit code test failure
                try {
                    if(!verify_max_requests_in_flight())
                        return derived().do_eof();
                    
                    derived().run();
                } catch (...) { }
            }
    }; // end class beast_http_session

    // Handles a plain HTTP connection
    class plain_session
        : public beast_http_session<plain_session> 
        , public std::enable_shared_from_this<plain_session>
    {
        beast::tcp_stream stream_;

        public:      
            // Create the session
            plain_session(
                tcp_socket_t&& socket,
                std::shared_ptr<ssl::context> ctx,
                std::shared_ptr<http_plugin_state> plugin_state
                )
                : beast_http_session<plain_session>(plugin_state)
                , stream_(std::move(socket))
            {}

            beast::tcp_stream& stream() { return stream_; }

            // Start the asynchronous operation
            void run()
            {
                do_read();
            }

            void do_eof()
            {
                // Send a TCP shutdown
                beast::error_code ec;

                stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
                // At this point the connection is closed gracefully
            }

            bool is_secure() { return false; };

            bool allow_host(const http::request<http::string_body>& req) { 
                return eosio::allow_host(req, *this, plugin_state_);
            }

            static constexpr auto name() {
                return "plain_session";
            }
    }; // end class plain_session

    // Handles an SSL HTTP connection
    class ssl_session
        : public beast_http_session<ssl_session>
        , public std::enable_shared_from_this<ssl_session>
    {   
        beast::ssl_stream<beast::tcp_stream> stream_;

        public:
            // Create the session

            ssl_session(
                tcp_socket_t&& socket,
                std::shared_ptr<ssl::context> ctx,
                std::shared_ptr<http_plugin_state> plugin_state)
                : beast_http_session<ssl_session>(plugin_state)
                , stream_(std::move(socket), *ctx)
            { }


            beast::ssl_stream<beast::tcp_stream> &stream() { return stream_; }

            // Start the asynchronous operation

            void run()
            {
                auto self = shared_from_this();
                self->stream_.async_handshake(
                    ssl::stream_base::server,
                    self->buffer_.data(),
                    [self](beast::error_code ec, std::size_t bytes_used) {
                        self->on_handshake(ec, bytes_used);
                    }
                );
            }

            void on_handshake(beast::error_code ec, std::size_t bytes_used)
            {
                if(ec)
                    return fail(ec, "handshake");

                buffer_.consume(bytes_used);

                do_read();
            }

            void do_eof()
            {
                // Perform the SSL shutdown
                beast::error_code ec;
                stream_.shutdown(ec);
                on_shutdown(ec);
            }

            void on_shutdown(beast::error_code ec)
            {
                if(ec)
                    return fail(ec, "shutdown");
                // At this point the connection is closed gracefully
            }

            bool is_secure() { return true; }

            bool allow_host(const http::request<http::string_body>& req) { 
                return eosio::allow_host(req, *this, plugin_state_);
            }

            static constexpr auto name() {
                return "ssl_session";
            }
    }; // end class ssl_session


    // unix domain sockets
    class unix_socket_session 
        : public std::enable_shared_from_this<unix_socket_session>
        , public beast_http_session<unix_socket_session>
    {    
        
        // The socket used to communicate with the client.
        local_stream stream_;

        public:
            unix_socket_session(stream_protocol::socket&& sock, 
                            std::shared_ptr<ssl::context> ctx,
                            std::shared_ptr<http_plugin_state> plugin_state) 
            : beast_http_session(plugin_state)
            , stream_(std::move(sock)) 
            {  }

            virtual ~unix_socket_session() = default;

            bool allow_host(const http::request<http::string_body>& req) { 
                // TODO allow host make sense here ? 
                return true;
            }

            void do_eof()
            {
                // Send a TCP shutdown
                boost::system::error_code ec;
                stream_.socket().shutdown(stream_protocol::socket::shutdown_send, ec);
                // At this point the connection is closed gracefully
            }

            bool is_secure() { return false; };

            void run() {
                // catch any loose exceptions so that nodeos will return zero exit code
                do_read();
            }

            local_stream& stream() { return stream_; }
            
            static constexpr auto name() {
                return "unix_socket_session";
            }
    }; // end class unix_socket_session

} // end namespace