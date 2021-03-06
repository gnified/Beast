//
// Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BEAST_EXAMPLE_SERVER_WS_SYNC_PORT_HPP
#define BEAST_EXAMPLE_SERVER_WS_SYNC_PORT_HPP

#include "server.hpp"

#include <beast/core/multi_buffer.hpp>
#include <beast/websocket.hpp>
#include <functional>
#include <memory>
#include <ostream>
#include <thread>

namespace framework {

// The connection object holds the state of the connection
// including, most importantly, the socket or stream.
//
// `Stream` is the type of socket or stream used as the
// transport. Examples include boost::asio::ip::tcp::socket
// or `ssl_stream`.
//
template<class Derived>
class sync_ws_con
{
    Derived&
    impl()
    {
        return static_cast<Derived&>(*this);
    }

    // The string used to set the Server http field
    std::string server_name_;

    // The stream to use for logging
    std::ostream& log_;

    // A small unique integer for logging
    std::size_t id_;

    // The remote endpoint. We cache it here because
    // calls to remote_endpoint() can fail / throw.
    //
    endpoint_type ep_;

public:
    // Constructor
    template<class Callback>
    sync_ws_con(
        beast::string_view server_name,
        std::ostream& log,
        std::size_t id,
        endpoint_type const& ep,
        Callback const& cb)
        : server_name_(server_name)
        , log_(log)
        , id_(id)
        , ep_(ep)
    {
        cb(impl().ws());
    }

    // Run the connection.
    //
    void
    run()
    {
        // We run the do_accept function in its own thread,
        // and bind a shared pointer to the connection object
        // into the function. The last reference to the shared
        // pointer will go away when the thread exits, thus
        // destroying the connection object.
        //
        std::thread{
            &sync_ws_con::do_accept,
            impl().shared_from_this()
        }.detach();
    }

    // Run the connection from an already-received Upgrade request.
    //
    template<class Body, class Fields>
    void
    run(beast::http::request<Body, Fields>&& req)
    {
        BOOST_ASSERT(beast::websocket::is_upgrade(req));

        // We need to transfer ownership of the request object into
        // the lambda, but there's no C++14 lambda capture
        // so we have to write it out by manually specifying the lambda.
        //
        std::thread{
            lambda<Body, Fields>{
                impl().shared_from_this(),
                std::move(req)
        }}.detach();
    }

private:
    // This is the lambda used when launching a connection from
    // an already-received request. In C++14 we could simply use
    // a lambda capture but this example requires only C++11 so
    // we write out the lambda ourselves. This is similar to what
    // the compiler would generate anyway.
    //
    template<class Body, class Fields>
    class lambda
    {
        std::shared_ptr<sync_ws_con> self_;
        beast::http::request<Body, Fields> req_;
        
    public:
        // Constructor
        //
        // This is the equivalent of the capture section of the lambda.
        //
        lambda(
            std::shared_ptr<sync_ws_con> self,
            beast::http::request<Body, Fields>&& req)
            : self_(std::move(self))
            , req_(std::move(req))
        {
            BOOST_ASSERT(beast::websocket::is_upgrade(req_));
        }

        // Invoke the lambda
        //
        void
        operator()()
        {
            BOOST_ASSERT(beast::websocket::is_upgrade(req_));
            error_code ec;
            {
                // Move the message to the stack so we can get
                // rid of resources, otherwise it will linger
                // for the lifetime of the connection.
                //
                auto req = std::move(req_);

                // Call the overload of accept() which takes
                // the request by parameter, instead of reading
                // it from the network.
                //
                self_->impl().ws().accept_ex(req,
                    [&](beast::websocket::response_type& res)
                    {
                        res.insert(beast::http::field::server, self_->server_name_);
                    },
                    ec);
            }

            // Run the connection
            //
            self_->do_run(ec);
        }
    };

    void
    do_accept()
    {
        error_code ec;

        // Read the WebSocket upgrade request and attempt
        // to send back the response.
        //
        impl().ws().accept_ex(
            [&](beast::websocket::response_type& res)
            {
                res.insert(beast::http::field::server, server_name_);
            },
            ec);

        // Run the connection
        //
        do_run(ec);
    }

    void
    do_run(error_code ec)
    {
        // Helper lambda to report a failure
        //
        auto const fail =
            [&](std::string const& what, error_code ev)
            {
                if(ev != beast::websocket::error::closed)
                    log_ <<
                        "[#" << id_ << " " << ep_ << "] " <<
                        what << ": " << ev.message() << std::endl;
            };

        // Check for an error upon entry. This will
        // come from one of the two calls to accept()
        //
        if(ec)
        {
            fail("accept", ec);
            return;
        }

        // Loop, reading messages and echoing them back.
        //
        for(;;)
        {
            // This buffer holds the message. We place a one
            // megabyte limit on the size to prevent abuse.
            //
            beast::multi_buffer buffer{1024*1024};

            // Read the message
            //
            impl().ws().read(buffer, ec);

            if(ec)
                return fail("read", ec);

            // Set the outgoing message type. We will use
            // the same setting as the message we just read.
            //
            impl().ws().binary(impl().ws().got_binary());

            // Now echo back the message
            //
            impl().ws().write(buffer.data(), ec);

            if(ec)
                return fail("write", ec);
        }
    }
};

//------------------------------------------------------------------------------

class sync_ws_con_plain
    : public std::enable_shared_from_this<sync_ws_con_plain>
    , public base_from_member<beast::websocket::stream<socket_type>>
    , public sync_ws_con<sync_ws_con_plain>
{
public:
    template<class... Args>
    explicit
    sync_ws_con_plain(
        socket_type&& sock,
        Args&&... args)
        : base_from_member<beast::websocket::stream<socket_type>>(std::move(sock))
        , sync_ws_con<sync_ws_con_plain>(std::forward<Args>(args)...)
    {
    }

    beast::websocket::stream<socket_type>&
    ws()
    {
        return this->member;
    }
};

//------------------------------------------------------------------------------

/** A synchronous WebSocket @b PortHandler which implements echo.

    This is a port handler which accepts WebSocket upgrade HTTP
    requests and implements the echo protocol. All received
    WebSocket messages will be echoed back to the remote host.
*/
class ws_sync_port
{
    // The type of the on_stream callback
    using on_new_stream_cb = std::function<
        void(beast::websocket::stream<socket_type>&)>;

    server& instance_;
    std::ostream& log_;
    on_new_stream_cb cb_;

public:
    /** Constructor

        @param instance The server instance which owns this port

        @param log The stream to use for logging

        @param cb A callback which will be invoked for every new
        WebSocket connection. This provides an opportunity to change
        the settings on the stream before it is used. The callback
        should have this equivalent signature:
        @code
        template<class NextLayer>
        void callback(beast::websocket::stream<NextLayer>&);
        @endcode
        In C++14 this can be accomplished with a generic lambda. In
        C++11 it will be necessary to write out a lambda manually,
        with a templated operator().
    */
    template<class Callback>
    ws_sync_port(
        server& instance,
        std::ostream& log,
        Callback const& cb)
        : instance_(instance)
        , log_(log)
        , cb_(cb)
    {
    }

    /** Accept a TCP/IP connection.

        This function is called when the server has accepted an
        incoming connection.

        @param sock The connected socket.

        @param ep The endpoint of the remote host.
    */
    void
    on_accept(socket_type&& sock, endpoint_type ep)
    {
        // Create our connection object and run it
        //
        std::make_shared<sync_ws_con_plain>(
            std::move(sock),
            "ws_sync_port",
            log_,
            instance_.next_id(),
            ep,
            cb_
                )->run();
    }

    /** Accept a WebSocket upgrade request.

        This is used to accept a connection that has already
        delivered the handshake.

        @param stream The stream corresponding to the connection.

        @param ep The remote endpoint.

        @param req The upgrade request.
    */
    template<class Body, class Fields>
    void
    accept(
        socket_type&& sock,
        endpoint_type ep,
        beast::http::request<Body, Fields>&& req)
    {
        // Create the connection object and run it,
        // transferring ownershop of the ugprade request.
        //
        std::make_shared<sync_ws_con_plain>(
            std::move(sock),
            "ws_sync_port",
            log_,
            instance_.next_id(),
            ep,
            cb_
                )->run(std::move(req));
    }
};

} // framework

#endif
