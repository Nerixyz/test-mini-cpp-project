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

#define QT_NO_KEYWORDS 1

#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/make_printable.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <QDebug>
#include <QString>
#include <QByteArray>
#include <QLoggingCategory>
#include <QUrl>

#include <string>

#include <thread>

namespace beast = boost::beast;          // from <boost/beast.hpp>
namespace asio = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;        // from <boost/asio/ip/tcp.hpp>

class WebSocketPoolImpl;
struct WebSocketListener;

struct WebSocketOptions {
    QUrl url;
    std::vector<std::pair<std::string, std::string>> headers;
};

Q_LOGGING_CATEGORY(chatterinoWebsocket, "chatterino.websocket");

struct QByteArrayBuffer {
    struct QByteArrayHolder {
        QByteArray data;

        operator boost::asio::const_buffer() const
        {
            return {
                this->data.constData(),
                static_cast<size_t>(this->data.size()),
            };
        }
    };

    struct ConstIterator : public std::bidirectional_iterator_tag {
        // using iterator_category = std::bidirectional_iterator_tag;
        using value_type = QByteArrayHolder;
        using difference_type = ptrdiff_t;
        using pointer = const QByteArrayHolder *;
        using reference = const QByteArrayHolder &;

        constexpr ConstIterator() noexcept = default;

        constexpr explicit ConstIterator(pointer ptr) noexcept
            : ptr(ptr)
        {
        }

        [[nodiscard]] constexpr reference operator*() const noexcept
        {
            return *ptr;
        }

        [[nodiscard]] constexpr pointer operator->() const noexcept
        {
            return ptr;
        }

        constexpr ConstIterator &operator++() noexcept
        {
            ++ptr;
            return *this;
        }

        constexpr ConstIterator operator++(int) noexcept
        {
            ConstIterator tmp = *this;
            ++ptr;
            return tmp;
        }

        constexpr ConstIterator &operator--() noexcept
        {
            --ptr;
            return *this;
        }

        constexpr ConstIterator operator--(int) noexcept
        {
            ConstIterator tmp = *this;
            --ptr;
            return tmp;
        }

        [[nodiscard]] constexpr auto operator==(
            const ConstIterator &rhs) const noexcept
        {
            return this->ptr == rhs.ptr;
        }

        const QByteArrayHolder *ptr = nullptr;
    };

    using value_type = QByteArrayHolder;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = QByteArrayHolder *;
    using const_pointer = const QByteArrayHolder *;
    using reference = QByteArrayHolder &;
    using const_reference = const QByteArrayHolder &;
    using iterator = ConstIterator;
    using const_iterator = ConstIterator;

    QByteArrayBuffer(QByteArray ba)
        : holder({std::move(ba)})
    {
    }

    const_iterator begin() const
    {
        return ConstIterator(&this->holder);
    }

    const_iterator end() const
    {
        return ConstIterator((&this->holder) + 1);
    }

    QByteArray data() const
    {
        return this->holder.data;
    }

    QByteArrayHolder holder;
};


class WebSocketConnection
{
public:
    WebSocketConnection(WebSocketOptions options, int id,
                        std::unique_ptr<WebSocketListener> listener,
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
    virtual void sendText(const QByteArray &data) = 0;

    /// Send or queue a binary message.
    ///
    /// Can be called from any thread.
    virtual void sendBinary(const QByteArray &data) = 0;

protected:
    /// Reset and notify the parent and listener (if possible).
    ///
    /// - If the listener is set, notify it about a close event.
    /// - If the parent is set, notify it about a closed connection.
    /// - Set the listener and parent to (the equivalent of) nullptr.
    void detach();

    WebSocketOptions options;
    // nullable, used for signalling a disconnect
    std::unique_ptr<WebSocketListener> listener;
    // nullable, used for signalling a disconnect
    WebSocketPoolImpl *pool;

    boost::asio::ip::tcp::resolver resolver;

    std::deque<std::pair<bool, QByteArrayBuffer>> queuedMessages;
    bool isSending = false;
    bool isClosing = false;
    int id = 0;

    boost::beast::flat_buffer readBuffer;

    friend QDebug operator<<(QDebug dbg, const WebSocketConnection &conn);
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

    void sendText(const QByteArray &data);
    void sendBinary(const QByteArray &data);
    void close();

private:
    std::weak_ptr<WebSocketConnection> conn;
};

struct WebSocketListener {
    virtual ~WebSocketListener() = default;

    /// A text message was received.
    ///
    /// This function is called from the websocket thread.
    virtual void onTextMessage(QByteArray data) = 0;

    /// A binary message was received.
    ///
    /// This function is called from the websocket thread.
    virtual void onBinaryMessage(QByteArray data) = 0;

    /// The websocket was closed.
    ///
    /// This function is called from the websocket thread.
    /// @param self The allocated listener (i.e. `self.get() == this`). Be
    ///             careful where this is destroyed. Once `self` is destroyed,
    ///             the instance of this class will be destroyed.
    virtual void onClose(std::unique_ptr<WebSocketListener> self) = 0;
};

class WebSocketPool
{
public:
    WebSocketPool();
    ~WebSocketPool();

    [[nodiscard]] WebSocketHandle createSocket(
        WebSocketOptions options, std::unique_ptr<WebSocketListener> listener);

private:
    std::unique_ptr<WebSocketPoolImpl> impl;
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

WebSocketConnection::WebSocketConnection(
    WebSocketOptions options, int id,
    std::unique_ptr<WebSocketListener> listener, WebSocketPoolImpl *pool,
    boost::asio::io_context &ioc)
    : options(std::move(options))
    , listener(std::move(listener))
    , pool(pool)
    , resolver(boost::asio::make_strand(ioc))
    , id(id)
{
    qCDebug(chatterinoWebsocket) << *this << "Created";
}

WebSocketConnection::~WebSocketConnection()
{
    assert(!this->listener && !this->pool);
    qCDebug(chatterinoWebsocket) << *this << "Destroyed";
}

QDebug operator<<(QDebug dbg, const WebSocketConnection &conn)
{
    QDebugStateSaver state(dbg);

    dbg.noquote().nospace()
        << '[' << conn.id << '|' << conn.options.url.toDisplayString() << ']';

    return dbg;
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
        qCDebug(chatterinoWebsocket) << *this << "Detached";
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

    void sendText(const QByteArray &data) final;
    void sendBinary(const QByteArray &data) final;

protected:
    Derived *derived();

    void fail(boost::system::error_code ec, QStringView op);
    void doWsHandshake();

    void closeImpl();
    void trySend();

    Stream stream;

private:
    // This is private to ensure only `Derived` can construct this class.
    WebSocketConnectionHelper(WebSocketOptions options, int id,
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

    boost::asio::cancellation_signal readCancellation;

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

    TlsWebSocketConnection(WebSocketOptions options, int id,
                           std::unique_ptr<WebSocketListener> listener,
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
    WebSocketOptions options, int id,
    std::unique_ptr<WebSocketListener> listener, WebSocketPoolImpl *pool,
    asio::io_context &ioc, Stream stream)
    : WebSocketConnection(std::move(options), id, std::move(listener), pool,
                          ioc)
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
    auto host = this->options.url.host(QUrl::FullyEncoded).toStdString();
    if constexpr (requires { this->derived()->setupStream(host); })
    {
        if (!this->derived()->setupStream(host))
        {
            return;
        }
    }

    this->resolver.async_resolve(
        host, std::to_string(this->options.url.port(Derived::DEFAULT_PORT)),
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
void WebSocketConnectionHelper<Derived, Inner>::sendText(const QByteArray &data)
{
    this->post([self{this->shared_from_this()}, data] {
        self->queuedMessages.emplace_back(true, data);
        self->trySend();
    });
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::sendBinary(
    const QByteArray &data)
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
        this->fail(ec, u"resolve");
        return;
    }

    qCDebug(chatterinoWebsocket) << *this << "Resolved host";

    this->stream.control_callback(
        [self{this->weak_from_this()}](beast::websocket::frame_type ty,
                                       auto /* data */) {
            if (ty == beast::websocket::frame_type::close)
            {
                auto strong = self.lock();
                if (strong && !strong->isClosing)
                {
                    qCDebug(chatterinoWebsocket)
                        << *strong << "Received close frame";
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
        this->fail(ec, u"TCP handshake");
        return;
    }

    qCDebug(chatterinoWebsocket) << *this << "TCP handshake done";
    this->options.url.setPort(ep.port());

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
            bool hasUa = false;
            for (const auto &[key, value] : this->options.headers)
            {
                // TODO(Qt 6.5): Use QUtf8StringView
                QLatin1StringView keyView(key.c_str());
                if (QLatin1StringView("user-agent")
                        .compare(keyView, Qt::CaseInsensitive) == 0)
                {
                    hasUa = true;
                }

                try
                {
                    // this can fail if the key or value exceed the maximum size
                    req.set(key, value);
                }
                catch (const boost::system::system_error &err)
                {
                    qCWarning(chatterinoWebsocket)
                        << "Invalid header - name:" << QUtf8StringView(key)
                        << "value:" << QUtf8StringView(value)
                        << "error:" << QUtf8StringView(err.what());
                }
            }

            // default UA
            if (!hasUa)
            {
                auto ua = QStringLiteral("Chatterino/1")
                              .toStdString();
                req.set(beast::http::field::user_agent, ua);
            }
        },
    });

    auto host = this->options.url.host(QUrl::FullyEncoded).toStdString() + ':' +
                std::to_string(this->options.url.port(Derived::DEFAULT_PORT));
    auto path = this->options.url.path(QUrl::FullyEncoded).toStdString();
    if (path.empty())
    {
        path = "/";
    }
    this->stream.async_handshake(
        host, path,
        beast::bind_front_handler(&WebSocketConnectionHelper::onWsHandshake,
                                  this->shared_from_this()));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onWsHandshake(
    boost::system::error_code ec)
{
    if (!this->listener || this->isClosing)
    {
        return;
    }
    if (ec)
    {
        this->fail(ec, u"WS handshake");
        return;
    }

    qCDebug(chatterinoWebsocket)
        << *this << "WS handshake done" << this->stream.is_open();

    this->trySend();
    qDebug() << "queue read";
    this->stream.async_read(
        this->readBuffer,
        asio::bind_cancellation_slot(
            this->readCancellation.slot(),
            beast::bind_front_handler(&WebSocketConnectionHelper::onReadDone,
                                      this->shared_from_this())));
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::onReadDone(
    boost::system::error_code ec, size_t bytesRead)
{
    qDebug() << "enter read-done";
    if (!this->listener || this->isClosing)
    {
        qDebug() << "leave read-done";
        return;
    }
    if (ec)
    {
        this->fail(ec, u"read");
        qDebug() << "leave read-done";
        return;
    }

    // XXX: this copies - we could read directly into a QByteArray
    QByteArray data{
        static_cast<const char *>(this->readBuffer.cdata().data()),
        static_cast<QByteArray::size_type>(bytesRead),
    };
    this->readBuffer.consume(bytesRead);

    if (this->stream.got_text())
    {
        this->listener->onTextMessage(std::move(data));
    }
    else
    {
        this->listener->onBinaryMessage(std::move(data));
    }

    qDebug() << "queue read";
    this->stream.async_read(
        this->readBuffer,
        asio::bind_cancellation_slot(
            this->readCancellation.slot(),
            beast::bind_front_handler(&WebSocketConnectionHelper::onReadDone,
                                      this->shared_from_this())));
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
        this->fail(ec, u"write");
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
        this->queuedMessages.front().second,
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

    qCDebug(chatterinoWebsocket) << *this << "Closing...";

    // cancel all pending operations
    this->resolver.cancel();
    beast::get_lowest_layer(this->stream).cancel();
    this->readCancellation.emit(asio::cancellation_type::terminal);

    this->stream.async_close(
        beast::websocket::close_code::normal,
        [this, lifetime{this->shared_from_this()}](auto ec) {
            qDebug() << "enter close-cb";
            if (ec)
            {
                qCWarning(chatterinoWebsocket) << *this << "Failed to close"
                                               << QUtf8StringView(ec.message());
                // make sure we cancel all operations
                beast::get_lowest_layer(this->stream).cancel();
            }
            else
            {
                qCDebug(chatterinoWebsocket) << *this << "Closed";
            }
            this->detach();
            qDebug() << "leave close-cb";
        });
}

template <typename Derived, typename Inner>
void WebSocketConnectionHelper<Derived, Inner>::fail(
    boost::system::error_code ec, QStringView op)
{
    qCWarning(chatterinoWebsocket)
        << *this << "Failed:" << op << QUtf8StringView(ec.message());
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
    this->readCancellation.emit(asio::cancellation_type::terminal);
    this->detach();
}

// MARK: TlsWebSocketConnection

TlsWebSocketConnection::TlsWebSocketConnection(
    WebSocketOptions options, int id,
    std::unique_ptr<WebSocketListener> listener, WebSocketPoolImpl *pool,
    asio::io_context &ioc, asio::ssl::context &ssl)
    : WebSocketConnectionHelper(std::move(options), id, std::move(listener),
                                pool, ioc, Stream{asio::make_strand(ioc), ssl})
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
                   u"Setting SNI hostname");
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
                this->fail(ec, u"TLS handshake");
                return;
            }

            qCDebug(chatterinoWebsocket)
                << *this << "TLS handshake done, using"
                << ::SSL_get_version(this->stream.next_layer().native_handle());
            this->doWsHandshake();
        });
}

WebSocketPool::WebSocketPool() = default;
WebSocketPool::~WebSocketPool() = default;

WebSocketHandle WebSocketPool::createSocket(
    WebSocketOptions options, std::unique_ptr<WebSocketListener> listener)
{
    if (!this->impl)
    {
        this->impl = std::make_unique<WebSocketPoolImpl>();
    }
    if (this->impl->closing)
    {
        return {{}};
    }

    std::shared_ptr<WebSocketConnection> conn = std::make_shared<TlsWebSocketConnection>(
            std::move(options), this->impl->nextID++, std::move(listener),
            this->impl.get(), this->impl->ioc, this->impl->ssl);

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

WebSocketHandle::WebSocketHandle(
    std::weak_ptr<WebSocketConnection> conn)
    : conn(std::move(conn))
{
}

WebSocketHandle::~WebSocketHandle()
{
    this->close();
}

void WebSocketHandle::close()
{
    qCDebug(chatterinoWebsocket) << "outer close";
    auto strong = this->conn.lock();
    if (strong)
    {
        strong->close();
    }
}

void WebSocketHandle::sendText(const QByteArray &data)
{
    auto strong = this->conn.lock();
    if (strong)
    {
        strong->sendText(data);
    }
}

void WebSocketHandle::sendBinary(const QByteArray &data)
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
    if (ec)
    {
        qCWarning(chatterinoWebsocket) << "Failed to set SSL context options"
                                       << QString::fromStdString(ec.message());
    }

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

    void onTextMessage(QByteArray data) override
    {
        messages.emplace_back(true, std::move(data));
    }

    void onBinaryMessage(QByteArray data) override
    {
        messages.emplace_back(false, std::move(data));
    }

    OnceFlag &closeFlag;
    std::vector<std::pair<bool, QByteArray>> messages;
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
    auto it = pool.createSocket({.url = QUrl("wss://127.0.0.1:9050"),}, std::make_unique<Listener>(closeFlag));

    it.sendBinary(QByteArray(1 << 15, 'A'));
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
