//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: WebSocket SSL client, asynchronous
//
//------------------------------------------------------------------------------

#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/make_printable.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>

#include <string>

#include <thread>

namespace beast = boost::beast;          // from <boost/beast.hpp>
namespace http = beast::http;            // from <boost/beast/http.hpp>
namespace websocket = beast::websocket;  // from <boost/beast/websocket.hpp>
namespace asio = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;        // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;        // from <boost/asio/ip/tcp.hpp>

//------------------------------------------------------------------------------

// Report a failure
void fail(beast::error_code ec, char const *what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

class WebSocketPoolImpl;
struct WebSocketListener;

class WebSocketConnection
{
public:
    WebSocketConnection(int id, std::unique_ptr<WebSocketListener> listener,
                        WebSocketPoolImpl *pool, boost::asio::io_context &ioc);
    virtual ~WebSocketConnection();

    WebSocketConnection(const WebSocketConnection &) = delete;
    WebSocketConnection(WebSocketConnection &&) = delete;
    WebSocketConnection &operator=(const WebSocketConnection &) = delete;
    WebSocketConnection &operator=(WebSocketConnection &&) = delete;

    /// Start connecting.
    ///
    /// Must be called from the desired executor.
    virtual void run() = 0;

    /// Close this connection gracefully (if possible).
    ///
    /// Can be called from any thread.
    virtual void close() = 0;

    /// Send or queue a text message.
    ///
    /// Can be called from any thread.
    virtual void sendText(const std::string &data) = 0;

    /// Send or queue a binary message.
    ///
    /// Can be called from any thread.
    virtual void sendBinary(const std::string &data) = 0;

protected:
    /// Reset and notify the parent and listener (if possible).
    ///
    /// - If the listener is set, notify it about a close event.
    /// - If the parent is set, notify it about a closed connection.
    /// - Set the listener and parent to (the equivalent of) nullptr.
    void detach();

    boost::asio::ip::tcp::resolver resolver;

    std::deque<std::pair<bool, std::string>> queuedMessages;
    bool isSending = false;
    bool isClosing = false;
    std::unique_ptr<WebSocketListener> listener;
    WebSocketPoolImpl *pool;
    int id = 0;

    boost::beast::flat_buffer readBuffer;
};

class WebSocketHandle
{
public:
    WebSocketHandle() = default;
    WebSocketHandle(std::weak_ptr<WebSocketConnection> conn);
    ~WebSocketHandle();

    WebSocketHandle(const WebSocketHandle &) = delete;
    WebSocketHandle(WebSocketHandle &&) = default;
    WebSocketHandle &operator=(const WebSocketHandle &) = delete;
    WebSocketHandle &operator=(WebSocketHandle &&) = default;

    void sendText(const std::string &data);
    void sendBinary(const std::string &data);
    void close();

private:
    std::weak_ptr<WebSocketConnection> conn;
};

struct WebSocketListener {
    virtual ~WebSocketListener() = default;

    virtual void onTextMessage() = 0;

    virtual void onBinaryMessage() = 0;

    virtual void onClose(std::unique_ptr<WebSocketListener> self) = 0;
};

class WebSocketConnection;

class WebSocketPoolImpl
{
public:
    WebSocketPoolImpl();
    ~WebSocketPoolImpl();

    WebSocketPoolImpl(const WebSocketPoolImpl &) = delete;
    WebSocketPoolImpl(WebSocketPoolImpl &&) = delete;
    WebSocketPoolImpl &operator=(const WebSocketPoolImpl &) = delete;
    WebSocketPoolImpl &operator=(WebSocketPoolImpl &&) = delete;

    void removeConnection(WebSocketConnection *conn);

    std::unique_ptr<std::thread> ioThread;
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work;

    std::vector<std::shared_ptr<WebSocketConnection>> connections;
    std::mutex connectionMutex;

    bool closing = false;
    int nextID = 1;
};

class WebSocketPool
{
public:
    WebSocketPool();
    ~WebSocketPool();

    [[nodiscard]] WebSocketHandle createSocket(
        std::unique_ptr<WebSocketListener> listener);

private:
    std::unique_ptr<WebSocketPoolImpl> impl;
};

WebSocketConnection::WebSocketConnection(
    int id, std::unique_ptr<WebSocketListener> listener,
    WebSocketPoolImpl *pool, boost::asio::io_context &ioc)
    : resolver(boost::asio::make_strand(ioc))
    , listener(std::move(listener))
    , pool(pool)
    , id(id)
{
    std::cout << "Created\n";
}

WebSocketConnection::~WebSocketConnection()
{
    std::cout << "Destroyed" << std::endl;
}

void WebSocketConnection::detach()
{
    bool anyWork = this->listener != nullptr || this->pool != nullptr;
    if (this->listener)
    {
        this->listener->onClose(std::move(this->listener));
    }
    if (this->pool)
    {
        this->pool->removeConnection(this);
        this->pool = nullptr;
    }

    if (anyWork)
    {
        std::cout << "Detached\n";
    }
}

template <typename Derived, typename Inner>
class WebSocketConnectionHelper : public WebSocketConnection,
                                  public std::enable_shared_from_this<
                                      WebSocketConnectionHelper<Derived, Inner>>
{
public:
    using Stream = boost::beast::websocket::stream<Inner>;

    void post(auto &&fn);

    void run() final;
    void close() final;

    void sendText(const std::string &data) final;
    void sendBinary(const std::string &data) final;

protected:
    Derived *derived();

    void fail(boost::system::error_code ec, std::string op);
    void doWsHandshake();

    void closeImpl();
    void trySend();

    Stream stream;

private:
    // This is private to ensure only `Derived` can construct this class.
    WebSocketConnectionHelper(int id,
                              std::unique_ptr<WebSocketListener> listener,
                              WebSocketPoolImpl *pool,
                              boost::asio::io_context &ioc, Stream stream);

    void onResolve(boost::system::error_code ec,
                   const boost::asio::ip::tcp::resolver::results_type &results);
    void onTcpHandshake(
        boost::system::error_code ec,
        const boost::asio::ip::tcp::resolver::endpoint_type &ep);
    void onWsHandshake(boost::system::error_code ec);

    void onReadDone(boost::system::error_code ec, size_t bytesRead);
    void onWriteDone(boost::system::error_code ec, size_t bytesWritten);

    void forceStop();  // assumes the socket has been closed

    friend Derived;
};

/// A WebSocket connection over TLS (wss://).
class TlsWebSocketConnection
    : public WebSocketConnectionHelper<
          TlsWebSocketConnection,
          boost::asio::ssl::stream<boost::beast::tcp_stream>>
{
public:
    static constexpr int DEFAULT_PORT = 443;

    TlsWebSocketConnection(int id, std::unique_ptr<WebSocketListener> listener,
                           WebSocketPoolImpl *pool,
                           boost::asio::io_context &ioc,
                           boost::asio::ssl::context &ssl);

protected:
    bool setupStream(const std::string &host);
    void afterTcpHandshake();

    friend WebSocketConnectionHelper<
        TlsWebSocketConnection,
        boost::asio::ssl::stream<boost::beast::tcp_stream>>;
};

// MARK: WebSocketConnectionHelper

template <typename Derived, typename Inner>
WebSocketConnectionHelper<Derived, Inner>::WebSocketConnectionHelper(
    int id, std::unique_ptr<WebSocketListener> listener,
    WebSocketPoolImpl *pool, asio::io_context &ioc, Stream stream)
    : WebSocketConnection(id, std::move(listener), pool, ioc)
    , stream(std::move(stream))
{
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::post(auto &&fn)
{
    asio::post(this->stream.get_executor(), std::forward<decltype(fn)>(fn));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::run()
{
    std::string host = "127.0.0.1";

    if constexpr (requires { this->stream.next_layer().native_handle(); })
    {
        if (::SSL_set_tlsext_host_name(
                this->stream.next_layer().native_handle(), host.c_str()) == 0)
        {
            this->fail({static_cast<int>(::ERR_get_error()),
                        asio::error::get_ssl_category()},
                       "Setting SNI hostname");
            return;
        }
    }

    this->resolver.async_resolve(
        host, "9050",
        beast::bind_front_handler(&WebSocketConnectionHelper::onResolve,
                                  this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::close()
{
    this->post([self{this->shared_from_this()}] {
        self->closeImpl();
    });
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::sendText(
    const std::string &data)
{
    this->post([self{this->shared_from_this()}, data] {
        self->queuedMessages.emplace_back(true, data);
        self->trySend();
    });
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::sendBinary(
    const std::string &data)
{
    this->post([self{this->shared_from_this()}, data] {
        self->queuedMessages.emplace_back(false, data);
        self->trySend();
    });
}

template <typename Derived, typename Inner>
Derived *WebSocketConnectionHelper<Derived, Inner>::derived()
{
    return static_cast<Derived *>(this);
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onResolve(
    boost::system::error_code ec,
    const asio::ip::tcp::resolver::results_type &results)
{
    if (ec)
    {
        this->fail(ec, "resolve");
        return;
    }

    std::cout << "Resolved host\n";

    this->stream.control_callback(
        [self{this->weak_from_this()}](beast::websocket::frame_type ty,
                                       auto /* data */) {
            if (ty == beast::websocket::frame_type::close)
            {
                auto strong = self.lock();
                if (strong && !strong->isClosing)
                {
                    std::cout << "Received close frame\n";
                    strong->forceStop();
                }
            }
        });

    beast::get_lowest_layer(this->stream)
        .expires_after(std::chrono::seconds{30});
    beast::get_lowest_layer(this->stream)
        .async_connect(results, beast::bind_front_handler(
                                    &WebSocketConnectionHelper::onTcpHandshake,
                                    this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onTcpHandshake(
    boost::system::error_code ec,
    const asio::ip::tcp::resolver::endpoint_type &ep)
{
    if (ec)
    {
        this->fail(ec, "TCP handshake");
        return;
    }

    std::cout << "TCP handshake done\n";

    this->derived()->afterTcpHandshake();
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::doWsHandshake()
{
    beast::get_lowest_layer(this->stream).expires_never();
    this->stream.set_option(beast::websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    this->stream.set_option(beast::websocket::stream_base::decorator{
        [this](beast::websocket::request_type &req) {
            req.set(beast::http::field::user_agent, "something");
        },
    });

    std::string host = "127.0.0.1:9050";
    std::string path = "/";
    this->stream.async_handshake(
        host, path,
        beast::bind_front_handler(&WebSocketConnectionHelper::onWsHandshake,
                                  this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onWsHandshake(
    boost::system::error_code ec)
{
    if (this->isClosing)
    {
        return;
    }
    if (ec)
    {
        this->fail(ec, "WS handshake");
        return;
    }

    std::cout << "WS handshake done\n";

    this->trySend();
    std::cout << "queue read\n";
    this->stream.async_read(
        this->readBuffer,
        beast::bind_front_handler(&WebSocketConnectionHelper::onReadDone,
                                  this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onReadDone(
    boost::system::error_code ec, size_t bytesRead)
{
    std::cout << "enter read-done\n";
    if (!this->listener || this->isClosing)
    {
        std::cout << "leave read-done\n";
        return;
    }
    if (ec)
    {
        this->fail(ec, "read");
        std::cout << "leave read-done\n";
        return;
    }

    this->readBuffer.consume(bytesRead);

    std::cout << "queue read\n";
    this->stream.async_read(
        this->readBuffer,
        beast::bind_front_handler(&WebSocketConnectionHelper::onReadDone,
                                  this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onWriteDone(
    boost::system::error_code ec, size_t /*bytesWritten*/)
{
    if (!this->queuedMessages.empty())
    {
        this->queuedMessages.pop_front();
    }
    else
    {
        assert(false);
    }
    this->isSending = false;

    if (ec)
    {
        this->fail(ec, "write");
        return;
    }

    this->trySend();
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::trySend()
{
    if (this->queuedMessages.empty() || this->isSending ||
        !this->stream.is_open())
    {
        return;
    }

    this->isSending = true;
    this->stream.text(this->queuedMessages.front().first);
    this->stream.async_write(
        asio::buffer(this->queuedMessages.front().second),
        beast::bind_front_handler(&WebSocketConnectionHelper::onWriteDone,
                                  this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::closeImpl()
{
    if (this->isClosing)
    {
        return;
    }
    this->isClosing = true;

    std::cout << "Closing...\n";

    // cancel all pending operations
    this->resolver.cancel();
    beast::get_lowest_layer(this->stream).cancel();

    this->stream.async_close(
        beast::websocket::close_code::normal,
        [this, lifetime{this->shared_from_this()}](auto ec) {
            std::cout << "enter close-cb\n";
            if (ec)
            {
                std::cout << "Failed to close\n";
                // make sure we cancel all operations
                beast::get_lowest_layer(this->stream).cancel();
            }
            else
            {
                std::cout << "Closed\n";
            }
            this->detach();
            std::cout << "leave close-cb\n";
        });
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::fail(
    boost::system::error_code ec, std::string op)
{
    std::cout << "Failed: " << op << ' ' << ec.message() << std::endl;
    if (this->stream.is_open())
    {
        this->closeImpl();
    }
    this->detach();
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::forceStop()
{
    this->isClosing = true;
    this->resolver.cancel();
    beast::get_lowest_layer(this->stream).cancel();
    this->detach();
}

// MARK: TlsWebSocketConnection

TlsWebSocketConnection::TlsWebSocketConnection(
    int id, std::unique_ptr<WebSocketListener> listener,
    WebSocketPoolImpl *pool, asio::io_context &ioc, asio::ssl::context &ssl)
    : WebSocketConnectionHelper(id, std::move(listener), pool, ioc,
                                Stream{asio::make_strand(ioc), ssl})
{
}

bool TlsWebSocketConnection::setupStream(const std::string &host)
{
    // Set SNI Hostname (many hosts need this to handshake successfully)
    if (::SSL_set_tlsext_host_name(this->stream.next_layer().native_handle(),
                                   host.c_str()) == 0)
    {
        this->fail({static_cast<int>(::ERR_get_error()),
                    asio::error::get_ssl_category()},
                   "Setting SNI hostname");
        return false;
    }
    return true;
}

void TlsWebSocketConnection::afterTcpHandshake()
{
    beast::get_lowest_layer(this->stream)
        .expires_after(std::chrono::seconds{30});
    this->stream.next_layer().async_handshake(
        asio::ssl::stream_base::client,
        [this,
         lifetime{this->shared_from_this()}](boost::system::error_code ec) {
            if (ec)
            {
                this->fail(ec, "TLS handshake");
                return;
            }

            std::cout << "TLS handshake done, using"
                      << ::SSL_get_version(
                             this->stream.next_layer().native_handle())
                      << std::endl;
            this->doWsHandshake();
        });
}

WebSocketPool::WebSocketPool() = default;
WebSocketPool::~WebSocketPool() = default;

WebSocketHandle WebSocketPool::createSocket(
    std::unique_ptr<WebSocketListener> listener)
{
    if (!this->impl)
    {
        this->impl = std::make_unique<WebSocketPoolImpl>();
    }
    if (this->impl->closing)
    {
        return {{}};
    }

    std::shared_ptr<WebSocketConnection> conn =
        std::make_shared<TlsWebSocketConnection>(
            this->impl->nextID++, std::move(listener), this->impl.get(),
            this->impl->ioc, this->impl->ssl);

    {
        std::unique_lock guard(this->impl->connectionMutex);
        this->impl->connections.push_back(conn);
    }

    boost::asio::post(this->impl->ioc, [conn] {
        conn->run();
    });

    return {conn};
}

// MARK: WebSocketHandle

WebSocketHandle::WebSocketHandle(std::weak_ptr<WebSocketConnection> conn)
    : conn(std::move(conn))
{
}

WebSocketHandle::~WebSocketHandle()
{
    this->close();
}

void WebSocketHandle::close()
{
    std::cout << "outer close\n";
    auto strong = this->conn.lock();
    if (strong)
    {
        strong->close();
    }
}

void WebSocketHandle::sendText(const std::string &data)
{
    auto strong = this->conn.lock();
    if (strong)
    {
        strong->sendText(data);
    }
}

void WebSocketHandle::sendBinary(const std::string &data)
{
    auto strong = this->conn.lock();
    if (strong)
    {
        strong->sendBinary(data);
    }
}

WebSocketPoolImpl::WebSocketPoolImpl()
    : ioc(1)
    , ssl(boost::asio::ssl::context::tls_client)
    , work(this->ioc.get_executor())
{
    boost::system::error_code ec;
    auto _ = this->ssl.set_options(
        boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 |
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::single_dh_use,
        ec);

    this->ioThread = std::make_unique<std::thread>([this] {
        this->ioc.run();
    });
}

WebSocketPoolImpl::~WebSocketPoolImpl()
{
    this->closing = true;
    this->work.reset();
    {
        std::lock_guard g(this->connectionMutex);
        for (const auto &conn : this->connections)
        {
            conn->close();
        }
    }
    if (!this->ioThread)
    {
        return;
    }

    // Set a maximum timeout on the close operations on all clients.
    // if (this->stoppedFlag.waitFor(std::chrono::milliseconds{1000}))
    {
        this->ioThread->join();
        return;
    }

    // qCWarning(chatterinoWebsocket)
    //     << "IO-Thread didn't finish after stopping, discard it";
    // // detach the thread so the destructor doesn't attempt any joining
    // this->ioThread->detach();
}

void WebSocketPoolImpl::removeConnection(WebSocketConnection *conn)
{
    std::lock_guard g(this->connectionMutex);
    std::erase_if(this->connections, [conn](const auto &v) {
        return v.get() == conn;
    });
}

class OnceFlag
{
public:
    OnceFlag() = default;

    void set()
    {
        {
            std::unique_lock guard(this->mutex);
            this->flag = true;
        }
        this->condvar.notify_all();
    }

    bool waitFor(std::chrono::milliseconds ms)
    {
        std::unique_lock lock(this->mutex);
        return this->condvar.wait_for(lock, ms, [this] {
            return this->flag;
        });
    }

private:
    std::mutex mutex;
    std::condition_variable condvar;
    bool flag = false;
};

struct Listener : public WebSocketListener {
    Listener(OnceFlag &f)
        : closeFlag(f)
    {
    }

    void onClose(std::unique_ptr<WebSocketListener> /*self*/) override
    {
        closeFlag.set();
    }

    void onTextMessage() override
    {
    }

    void onBinaryMessage() override
    {
    }

    OnceFlag &closeFlag;
};

//------------------------------------------------------------------------------

int main(int argc, char **argv)
{
    // Check command line arguments.
    if (argc != 3)
    {
        std::cerr << "Usage: websocket-client-async-ssl <host> <port> <text>\n"
                  << "Example:\n"
                  << "    websocket-client-async-ssl echo.websocket.org 443 "
                     "\"Hello, world!\"\n";
        return EXIT_FAILURE;
    }
    auto const host = argv[1];
    auto const port = argv[2];

    WebSocketPool pool;
    OnceFlag closeFlag;
    auto it = pool.createSocket(std::make_unique<Listener>(closeFlag));

    it.sendBinary(std::string(1 << 15, 'A'));
    it.sendText("foo");
    it.sendText("foo");
    it.sendText("foo");
    it.sendText("foo");
    it.sendText("foo");
    it.sendText("foo");
    it.sendText("/CLOSE");

    closeFlag.waitFor(std::chrono::milliseconds{1000});

    return EXIT_SUCCESS;
}
