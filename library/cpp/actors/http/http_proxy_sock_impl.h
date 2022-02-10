#pragma once 
 
#include "http.h" 
#include "http_proxy.h" 
 
namespace NHttp { 
 
struct TPlainSocketImpl : virtual public THttpConfig { 
    TIntrusivePtr<TSocketDescriptor> Socket; 
 
    TPlainSocketImpl() 
        : Socket(new TSocketDescriptor()) 
    {} 
 
    TPlainSocketImpl(TIntrusivePtr<TSocketDescriptor> socket) 
        : Socket(std::move(socket)) 
    {} 
 
    SOCKET GetRawSocket() const { 
        return static_cast<SOCKET>(Socket->Socket); 
    } 
 
    void SetNonBlock(bool nonBlock = true) noexcept { 
        try { 
            ::SetNonBlock(Socket->Socket, nonBlock); 
        } 
        catch (const yexception&) { 
        } 
    } 
 
    void SetTimeout(TDuration timeout) noexcept { 
        try { 
            ::SetSocketTimeout(Socket->Socket, timeout.Seconds(), timeout.MilliSecondsOfSecond()); 
        } 
        catch (const yexception&) { 
        } 
    } 
 
    void Shutdown() { 
        //Socket->Socket.ShutDown(SHUT_RDWR); // KIKIMR-3895 
        ::shutdown(Socket->Socket, SHUT_RDWR); 
    } 
 
    int Connect(const SocketAddressType& address) { 
        return Socket->Socket.Connect(&address); 
    } 
 
    static constexpr int OnConnect(bool&, bool&) {
        return 1;
    } 
 
    static constexpr int OnAccept(const TEndpointInfo&, bool&, bool&) {
        return 1;
    } 
 
    bool IsGood() { 
        int res; 
        GetSockOpt(Socket->Socket, SOL_SOCKET, SO_ERROR, res); 
        return res == 0; 
    } 
 
    int GetError() { 
        int res; 
        GetSockOpt(Socket->Socket, SOL_SOCKET, SO_ERROR, res); 
        return res; 
    } 
 
    ssize_t Send(const void* data, size_t size, bool&, bool&) {
        return Socket->Socket.Send(data, size); 
    } 
 
    ssize_t Recv(void* data, size_t size, bool&, bool&) {
        return Socket->Socket.Recv(data, size); 
    } 
}; 
 
struct TSecureSocketImpl : TPlainSocketImpl, TSslHelpers { 
    static TSecureSocketImpl* IO(BIO* bio) noexcept { 
        return static_cast<TSecureSocketImpl*>(BIO_get_data(bio)); 
    } 
 
    static int IoWrite(BIO* bio, const char* data, int dlen) noexcept { 
        BIO_clear_retry_flags(bio); 
        int res = IO(bio)->Socket->Socket.Send(data, dlen); 
        if (-res == EAGAIN) { 
            BIO_set_retry_write(bio); 
        } 
        return res; 
    } 
 
    static int IoRead(BIO* bio, char* data, int dlen) noexcept { 
        BIO_clear_retry_flags(bio); 
        int res = IO(bio)->Socket->Socket.Recv(data, dlen); 
        if (-res == EAGAIN) { 
            BIO_set_retry_read(bio); 
        } 
        return res; 
    } 
 
    static int IoPuts(BIO* bio, const char* buf) noexcept { 
        Y_UNUSED(bio); 
        Y_UNUSED(buf); 
        return -2; 
    } 
 
    static int IoGets(BIO* bio, char* buf, int size) noexcept { 
        Y_UNUSED(bio); 
        Y_UNUSED(buf); 
        Y_UNUSED(size); 
        return -2; 
    } 
 
    static long IoCtrl(BIO* bio, int cmd, long larg, void* parg) noexcept { 
        Y_UNUSED(larg); 
        Y_UNUSED(parg); 
 
        if (cmd == BIO_CTRL_FLUSH) { 
            IO(bio)->Flush(); 
            return 1; 
        } 
 
        return -2; 
    } 
 
    static int IoCreate(BIO* bio) noexcept { 
        BIO_set_data(bio, nullptr); 
        BIO_set_init(bio, 1); 
        return 1; 
    } 
 
    static int IoDestroy(BIO* bio) noexcept { 
        BIO_set_data(bio, nullptr); 
        BIO_set_init(bio, 0); 
        return 1; 
    } 
 
    static BIO_METHOD* CreateIoMethod() { 
        BIO_METHOD* method = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "SecureSocketImpl"); 
        BIO_meth_set_write(method, IoWrite); 
        BIO_meth_set_read(method, IoRead); 
        BIO_meth_set_puts(method, IoPuts); 
        BIO_meth_set_gets(method, IoGets); 
        BIO_meth_set_ctrl(method, IoCtrl); 
        BIO_meth_set_create(method, IoCreate); 
        BIO_meth_set_destroy(method, IoDestroy); 
        return method; 
    } 
 
    static BIO_METHOD* IoMethod() { 
        static BIO_METHOD* method = CreateIoMethod(); 
        return method; 
    } 
 
    TSslHolder<BIO> Bio; 
    TSslHolder<SSL_CTX> Ctx; 
    TSslHolder<SSL> Ssl; 
 
    TSecureSocketImpl() = default; 
 
    TSecureSocketImpl(TIntrusivePtr<TSocketDescriptor> socket) 
        : TPlainSocketImpl(std::move(socket)) 
    {} 
 
    void InitClientSsl() { 
        Bio.Reset(BIO_new(IoMethod()));
        BIO_set_data(Bio.Get(), this); 
        BIO_set_nbio(Bio.Get(), 1); 
        Ctx = CreateClientContext(); 
        Ssl = ConstructSsl(Ctx.Get(), Bio.Get()); 
        SSL_set_connect_state(Ssl.Get()); 
    } 
 
    void InitServerSsl(SSL_CTX* ctx) { 
        Bio.Reset(BIO_new(IoMethod()));
        BIO_set_data(Bio.Get(), this); 
        BIO_set_nbio(Bio.Get(), 1); 
        Ssl = ConstructSsl(ctx, Bio.Get()); 
        SSL_set_accept_state(Ssl.Get()); 
    } 
 
    void Flush() {} 
 
    ssize_t Send(const void* data, size_t size, bool& read, bool& write) {
        ssize_t res = SSL_write(Ssl.Get(), data, size); 
        if (res < 0) { 
            res = SSL_get_error(Ssl.Get(), res); 
            switch(res) { 
            case SSL_ERROR_WANT_READ: 
                read = true;
                return -EAGAIN;
            case SSL_ERROR_WANT_WRITE: 
                write = true;
                return -EAGAIN; 
            default: 
                return -EIO; 
            } 
        } 
        return res; 
    } 
 
    ssize_t Recv(void* data, size_t size, bool& read, bool& write) {
        ssize_t res = SSL_read(Ssl.Get(), data, size); 
        if (res < 0) { 
            res = SSL_get_error(Ssl.Get(), res); 
            switch(res) { 
            case SSL_ERROR_WANT_READ: 
                read = true;
                return -EAGAIN;
            case SSL_ERROR_WANT_WRITE: 
                write = true;
                return -EAGAIN; 
            default: 
                return -EIO; 
            } 
        } 
        return res; 
    } 
 
    int OnConnect(bool& read, bool& write) {
        if (!Ssl) { 
            InitClientSsl(); 
        } 
        int res = SSL_connect(Ssl.Get()); 
        if (res <= 0) {
            res = SSL_get_error(Ssl.Get(), res); 
            switch(res) { 
            case SSL_ERROR_WANT_READ: 
                read = true;
                return -EAGAIN;
            case SSL_ERROR_WANT_WRITE: 
                write = true;
                return -EAGAIN; 
            default: 
                return -EIO; 
            } 
        } 
        return res; 
    } 
 
    int OnAccept(const TEndpointInfo& endpoint, bool& read, bool& write) {
        if (!Ssl) { 
            InitServerSsl(endpoint.SecureContext.Get()); 
        } 
        int res = SSL_accept(Ssl.Get()); 
        if (res <= 0) {
            res = SSL_get_error(Ssl.Get(), res); 
            switch(res) { 
            case SSL_ERROR_WANT_READ: 
                read = true;
                return -EAGAIN;
            case SSL_ERROR_WANT_WRITE: 
                write = true;
                return -EAGAIN; 
            default: 
                return -EIO; 
            } 
        } 
        return res; 
    } 
}; 
 
} 
