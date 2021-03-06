//
// Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BEAST_EXAMPLE_SERVER_HTTP_ASYNC_PORT_HPP
#define BEAST_EXAMPLE_SERVER_HTTP_ASYNC_PORT_HPP

#include "server.hpp"

#include "http_base.hpp"
#include "rfc7231.hpp"
#include "service_list.hpp"
#include "write_msg.hpp"

#include <beast/core/flat_buffer.hpp>
#include <beast/http/dynamic_body.hpp>
#include <beast/http/parser.hpp>
#include <beast/http/read.hpp>
#include <beast/http/string_body.hpp>
#include <beast/http/write.hpp>
#include <memory>
#include <utility>
#include <ostream>

namespace framework {

// Base class for a type-erased, queued asynchronous HTTP write
//
struct queued_http_write
{
    virtual ~queued_http_write() = default;

    // When invoked, performs the write operation.
    virtual void invoke() = 0;
};

/*  This implements an object which, when invoked, writes an HTTP
    message asynchronously to the stream. These objects are used
    to form a queue of outgoing messages for pipelining. The base
    class type-erases the message so the queue can hold messsages
    of different types.
*/
template<
    class Stream,
    bool isRequest, class Body, class Fields,
    class Handler>
class queued_http_write_impl : public queued_http_write
{
    Stream& stream_;
    beast::http::message<isRequest, Body, Fields> msg_;
    Handler handler_;

public:
    // Constructor.
    //
    // Ownership of the message is transferred into the object
    //
    template<class DeducedHandler>
    queued_http_write_impl(
        Stream& stream,
        beast::http::message<isRequest, Body, Fields>&& msg,
        DeducedHandler&& handler)
        : stream_(stream)
        , msg_(std::move(msg))
        , handler_(std::forward<DeducedHandler>(handler))
    {
    }

    // Writes the stored message
    void
    invoke() override
    {
        async_write_msg(
            stream_,
            std::move(msg_),
            std::move(handler_));
    }
};

// This helper function creates a queued_http_write
// object and returns it inside a unique_ptr.
//
template<
    class Stream,
    bool isRequest, class Body, class Fields,
    class Handler>
std::unique_ptr<queued_http_write>
make_queued_http_write(
    Stream& stream,
    beast::http::message<isRequest, Body, Fields>&& msg,
    Handler&& handler)
{
    return std::unique_ptr<queued_http_write>{
        new queued_http_write_impl<
            Stream,
            isRequest, Body, Fields,
            typename std::decay<Handler>::type>{
                stream,
                std::move(msg),
                std::forward<Handler>(handler)}};
}

//------------------------------------------------------------------------------

/** An asynchronous HTTP connection.

    This base class implements an HTTP connection object using
    asynchronous calls.

    It uses the Curiously Recurring Template pattern (CRTP) where
    we refer to the derivd class in order to access the stream object
    to use for reading and writing. This lets the same class be used
    for plain and SSL stream objects.

    @tparam Services The list of services this connection will support.
*/
template<class Derived, class... Services>
class async_http_con : public http_base
{
    // This function lets us access members of the derived class
    Derived&
    impl()
    {
        return static_cast<Derived&>(*this);
    }

    // The stream to use for logging
    std::ostream& log_;

    // The services configured for the port
    service_list<Services...> const& services_;

    // A small unique integer for logging
    std::size_t id_;

    // The remote endpoint. We cache it here because
    // calls to remote_endpoint() can fail / throw.
    //
    endpoint_type ep_;

    // The buffer for performing reads
    beast::flat_buffer buffer_;

    // The parser for reading the requests
    boost::optional<beast::http::request_parser<beast::http::dynamic_body>> parser_;

    // This is the queue of outgoing messages
    std::vector<std::unique_ptr<queued_http_write>> queue_;

    // Indicates if we have a write active.
    bool writing_ = false;

protected:
    // The strand makes sure that our data is
    // accessed from only one thread at a time.
    //
    strand_type strand_;

public:
    // Constructor
    async_http_con(
        beast::string_view server_name,
        std::ostream& log,
        service_list<Services...> const& services,
        std::size_t id,
        endpoint_type const& ep)
        : http_base(server_name)
        , log_(log)
        , services_(services)
        , id_(id)
        , ep_(ep)
        // The buffer has a limit of 8192, otherwise
        // the server is vulnerable to a buffer attack.
        //
        , buffer_(8192)
        , strand_(impl().stream().get_io_service())
    {
    }

    // Called by the port after creating the object
    void
    run()
    {
        // Start reading the header for the first request.
        //
        do_read_header();
    }

private:
    // Perform an asynchronous read for the next request header
    //
    void
    do_read_header()
    {
        // On each read the parser needs to be destroyed and
        // recreated. We store it in a boost::optional to
        // achieve that.
        //
        // Arguments passed to the parser constructor are
        // forwarded to the message object. A single argument
        // is forwarded to the body constructor.
        //
        // We construct the dynamic body with a 1MB limit
        // to prevent vulnerability to buffer attacks.
        //
        parser_.emplace(1024 * 1024);

        // Read just the header
        beast::http::async_read_header(
            impl().stream(),
            buffer_,
            *parser_,
            strand_.wrap(std::bind(
                &async_http_con::on_read_header,
                impl().shared_from_this(),
                std::placeholders::_1)));
    }

    // This lambda is passed to the service list to handle
    // the case of sending request objects of varying types.
    // In C++14 this is more easily accomplished using a generic
    // lambda, but we want C+11 compatibility so we manually
    // write the lambda out.
    //
    struct send_lambda
    {
        // holds "this"
        async_http_con& self_;

    public:
        // capture "this"
        explicit
        send_lambda(async_http_con& self)
            : self_(self)
        {
        }

        // sends a message
        template<class Body, class Fields>
        void
        operator()(beast::http::response<Body, Fields>&& res) const
        {
            self_.do_write(std::move(res));
        }
    };

    // Called when the header has been read in
    void
    on_read_header(error_code ec)
    {
        // On failure we just return, the shared_ptr that is bound
        // into the completion will go out of scope and eventually
        // we will get destroyed.
        //
        if(ec)
            return fail("on_read", ec);

        // The parser holds the request object,
        // at this point it only has the header in it.
        auto& req = parser_->get();

        send_lambda send{*this};

        // See if they are specifying Expect: 100-continue
        //
        if(rfc7231::is_expect_100_continue(req))
        {
            // They want to know if they should continue,
            // so send the appropriate response.
            //
            send(this->continue_100(req));
        }

        // Read the rest of the message, if any.
        //
        beast::http::async_read(
            impl().stream(),
            buffer_,
            *parser_,
            strand_.wrap(std::bind(
                &async_http_con::on_read,
                impl().shared_from_this(),
                std::placeholders::_1)));
    }

    // Called when the message is complete
    void
    on_read(error_code ec)
    {
        // Grab a reference to the request again
        auto& req = parser_->get();

        // Create a variable for our send
        // lambda since we use it more than once.
        //
        send_lambda send{*this};

        // Give each services a chance to handle the request
        //
        if(! services_.respond(
            impl().stream(),
            ep_,
            std::move(req),
            send))
        {
            // No service handled the request,
            // send a Bad Request result to the client.
            //
            send(this->bad_request(req));
        }
        else
        {
            // See if the service that handled the
            // response took ownership of the stream.
            //
            if(! impl().stream().lowest_layer().is_open())
            {
                // They took ownership so just return and
                // let this async_http_con object get destroyed.
                //
                return;
            }
        }

        // VFALCO Right now we do unlimited pipelining which
        //        can lead to unbounded resource consumption.
        //        A more sophisticated server might only issue
        //        this read when the queue is below some limit.
        //

        // Start reading another header
        do_read_header();
    }

    // This function either queues a message or
    // starts writing it if no other writes are taking place.
    //
    template<class Body, class Fields>
    void
    do_write(beast::http::response<Body, Fields>&& res)
    {
        // See if a write is in progress
        if(! writing_)
        {
            // An assert or two to keep things sane when
            // writing asynchronous code can be very helpful.
            BOOST_ASSERT(queue_.empty());

            // We're going to be writing so set the flag
            writing_ = true;

            // And now perform the write
            return async_write_msg(
                impl().stream(),
                std::move(res),
                strand_.wrap(std::bind(
                    &async_http_con::on_write,
                    impl().shared_from_this(),
                    std::placeholders::_1)));
        }

        // Queue is not empty, so append this message to the queue.
        // It will be sent late when the queue empties.
        //
        queue_.emplace_back(make_queued_http_write(
            impl().stream(),
            std::move(res),
            strand_.wrap(std::bind(
                &async_http_con::on_write,
                impl().shared_from_this(),
                std::placeholders::_1))));
    }

    // Called when a message finishes writing
    void
    on_write(error_code ec)
    {
        // Make sure our state is what we think it is
        BOOST_ASSERT(writing_);

        // On failure just log and return
        if(ec)
            return fail("on_write", ec);

        // See if the queue is empty
        if(queue_.empty())
        {
            // Queue was empty so clear the flag...
            writing_ = false;

            // ...and return
            return;
        }

        // Queue was not empty, so invoke the object
        // at the head of the queue. This will start
        // another wrte.
        queue_.front()->invoke();

        // Delete the item since we used it
        queue_.erase(queue_.begin());
    }

    // Called when a failure occurs
    //
    void
    fail(std::string what, error_code ec)
    {
        // Don't log end of stream or operation aborted
        // since those happen under normal circumstances.
        //
        if( ec != beast::http::error::end_of_stream &&
            ec != boost::asio::error::operation_aborted)
        {
            log_ <<
                "[#" << id_ << " " << ep_ << "] " <<
            what << ": " << ec.message() << std::endl;
        }
    }
};

//------------------------------------------------------------------------------

// This class represents an asynchronous HTTP connection which
// uses a plain TCP/IP socket (no encryption) as the stream.
//
template<class... Services>
class async_http_con_plain

    // Note that we give this object the enable_shared_from_this, and have
    // the base class call impl().shared_from_this(). The reason we do that
    // is so that the shared_ptr has the correct type. This lets the
    // derived class (this class) use its members in calls to std::bind,
    // without an ugly call to `dynamic_downcast` or other nonsense.
    //
    : public std::enable_shared_from_this<async_http_con_plain<Services...>>

    // We want the socket to be created before the base class so we use
    // the "base from member" idiom which Boost provides as a class.
    //
    , public base_from_member<socket_type>

    // Declare this base last now that everything else got set up first.
    //
    , public async_http_con<async_http_con_plain<Services...>, Services...>
{
public:
    // Construct the plain connection.
    //
    template<class... Args>
    async_http_con_plain(
        socket_type&& sock,
        Args&&... args)
        : base_from_member<socket_type>(std::move(sock))
        , async_http_con<async_http_con_plain<Services...>, Services...>(
            std::forward<Args>(args)...)
    {
    }

    // Returns the stream.
    // The base class calls this to obtain the object to
    // use for reading and writing HTTP messages.
    //
    socket_type&
    stream()
    {
        return this->member;
    }
};

//------------------------------------------------------------------------------

/*  An asynchronous HTTP port handler

    This type meets the requirements of @b PortHandler. It supports
    variable list of HTTP services in its template parameter list,
    and provides a synchronous connection implementation to service
*/
template<class... Services>
class http_async_port
{
    // Reference to the server instance that made us
    server& instance_;

    // The stream to log to
    std::ostream& log_;

    // The list of services connections created from this port will support
    service_list<Services...> services_;

public:
    // Constructor
    http_async_port(
        server& instance,
        std::ostream& log)
        : instance_(instance)
        , log_(log)
    {
    }

    /** Initialize a service

        Every service in the list must be initialized exactly once.

        @param args Optional arguments forwarded to the service
        constructor.

        @tparam Index The 0-based index of the service to initialize.

        @return A reference to the service list. This permits
        calls to be chained in a single expression.
    */
    template<std::size_t Index, class... Args>
    void
    init(error_code& ec, Args&&... args)
    {
        services_.template init<Index>(
            ec,
            std::forward<Args>(args)...);
    }

    /** Called by the server to provide ownership of the socket for a new connection

        @param sock The socket whose ownership is to be transferred

        @ep The remote endpoint
    */
    void
    on_accept(socket_type&& sock, endpoint_type ep)
    {
        // Create a plain http connection object
        // and transfer ownership of the socket.
        //
        std::make_shared<async_http_con_plain<Services...>>(
            std::move(sock),
            "http_async_port",
            log_,
            services_,
            instance_.next_id(),
            ep
                )->run();
    }
};

} // framework

#endif
