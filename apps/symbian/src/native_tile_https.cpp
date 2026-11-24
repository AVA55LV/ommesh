#include "native_tile_https.hpp"

#ifdef Q_OS_SYMBIAN
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <QtCore/QUrl>
#include "absl/time/clock.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "symbian/api/connectivity/http.h"
#include "symbian/api/connectivity/tcp_client.h"
#include "symbian/native_status.h"
#include "symbian/net/byte_stream.h"

#include "native_tile_ca.hpp"
#include "native_preview_ca.hpp"
#include "native_attachment_ca.hpp"

namespace {

namespace http = symbian::http;
namespace net = symbian::api::connectivity;

constexpr char kPreviewHost[] = "preview.ommesh.ru";
constexpr char kUserAgent[] = "OMMesh-Belle-SIS/0.2.43 (+https://github.com/cm4ker/ommesh)";
constexpr std::size_t kNativeIoLimit = 32 * 1024;

bool cancelled(const QAtomicInt* flag) {
    return flag && const_cast<QAtomicInt*>(flag)->fetchAndAddAcquire(0) != 0;
}

absl::Status tlsError(const char* operation, int code) {
    return absl::UnavailableError(std::string(operation) + ": " + std::to_string(code));
}

class NativeTlsStream final : public symbian::net::ByteStream {
public:
    NativeTlsStream(net::TcpClient tcp, const QAtomicInt* cancellation)
        : tcp_(std::move(tcp)), cancelled_(cancellation) {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&config_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_x509_crt_init(&roots_);
    }

    ~NativeTlsStream() override {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_x509_crt_free(&roots_);
    }

    absl::Status open(const char* host, const char* trustRoots, std::size_t trustRootsLength,
                      absl::Time deadline) {
        deadline_ = deadline;
        if (const auto status = checkDeadline(); !status.ok())
            return status;
        if (psa_crypto_init() != PSA_SUCCESS)
            return absl::UnavailableError("TLS crypto initialization failed");
        int code = mbedtls_ctr_drbg_seed(&random_, mbedtls_entropy_func, &entropy_, nullptr, 0);
        if (code != 0)
            return tlsError("TLS system entropy failed", code);
        code = mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (code != 0)
            return tlsError("TLS configuration failed", code);
        code = mbedtls_x509_crt_parse(&roots_, reinterpret_cast<const unsigned char*>(trustRoots),
                                     trustRootsLength);
        if (code != 0)
            return tlsError("TLS CA bundle rejected", code);
        mbedtls_ssl_conf_ca_chain(&config_, &roots_, nullptr);
        mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &random_);
        mbedtls_ssl_conf_min_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
        code = mbedtls_ssl_conf_alpn_protocols(&config_, protocols_);
        if (code == 0)
            code = mbedtls_ssl_setup(&ssl_, &config_);
        if (code == 0)
            code = mbedtls_ssl_set_hostname(&ssl_, host);
        if (code != 0)
            return tlsError("TLS setup failed", code);
        mbedtls_ssl_set_bio(&ssl_, this, send, receive, nullptr);
        do {
            if (const auto status = checkDeadline(); !status.ok())
                return status;
            code = mbedtls_ssl_handshake(&ssl_);
        } while (needsIo(code));
        if (!ioError_.ok())
            return ioError_;
        if (code != 0)
            return tlsError("TLS handshake failed", code);
        const char* protocol = mbedtls_ssl_get_alpn_protocol(&ssl_);
        if (protocol && std::string_view(protocol) != protocols_[0])
            return absl::FailedPreconditionError("Unexpected TLS application protocol");
        return absl::OkStatus();
    }

    absl::StatusOr<std::size_t> Read(std::span<std::uint8_t> bytes, absl::Time deadline) override {
        deadline_ = deadline;
        ioError_ = absl::OkStatus();
        int count;
        do {
            if (const auto status = checkDeadline(); !status.ok())
                return status;
            count = mbedtls_ssl_read(&ssl_, bytes.data(), bytes.size());
        } while (needsIo(count));
        if (!ioError_.ok())
            return ioError_;
        if (count > 0)
            return static_cast<std::size_t>(count);
        if (count == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return std::size_t{0};
        if (count == 0)
            return absl::DataLossError("TLS stream closed without an authenticated end");
        return tlsError("TLS read failed", count);
    }

    absl::Status Write(std::span<const std::uint8_t> bytes, absl::Time deadline) override {
        deadline_ = deadline;
        ioError_ = absl::OkStatus();
        while (!bytes.empty()) {
            if (const auto status = checkDeadline(); !status.ok())
                return status;
            const int count = mbedtls_ssl_write(&ssl_, bytes.data(), bytes.size());
            if (count > 0)
                bytes = bytes.subspan(static_cast<std::size_t>(count));
            else if (!needsIo(count))
                return ioError_.ok() ? tlsError("TLS write failed", count) : ioError_;
        }
        return absl::OkStatus();
    }

    void Close() override { tcp_.Close(); }

private:
    static bool needsIo(int code) {
        return code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    absl::Status checkDeadline() const {
        if (cancelled(cancelled_))
            return absl::CancelledError("HTTPS request cancelled");
        if (absl::Now() >= deadline_)
            return absl::DeadlineExceededError("HTTPS request timed out");
        return absl::OkStatus();
    }

    static int send(void* context, const unsigned char* bytes, std::size_t size) {
        auto& self = *static_cast<NativeTlsStream*>(context);
        self.ioError_ = self.checkDeadline();
        if (self.ioError_.ok())
            self.ioError_ = self.tcp_.Send(std::span(bytes, size), self.deadline_);
        return self.ioError_.ok() ? static_cast<int>(size) : MBEDTLS_ERR_NET_SEND_FAILED;
    }

    static int receive(void* context, unsigned char* bytes, std::size_t size) {
        auto& self = *static_cast<NativeTlsStream*>(context);
        self.ioError_ = self.checkDeadline();
        if (!self.ioError_.ok())
            return MBEDTLS_ERR_NET_RECV_FAILED;
        const auto until = std::min(self.deadline_, absl::Now() + absl::Milliseconds(500));
        auto received = self.tcp_.Receive(std::span(bytes, std::min(size, kNativeIoLimit)), until);
        if (received.ok())
            return static_cast<int>(*received);
        if (absl::IsDeadlineExceeded(received.status()))
            return MBEDTLS_ERR_SSL_WANT_READ;
        if (symbian::NativeErrorFromStatus(received.status()) == -25)
            return 0;
        self.ioError_ = received.status();
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    net::TcpClient tcp_;
    const QAtomicInt* cancelled_;
    absl::Time deadline_;
    absl::Status ioError_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config config_;
    mbedtls_ctr_drbg_context random_;
    mbedtls_entropy_context entropy_;
    mbedtls_x509_crt roots_;
    const char* protocols_[2] = {"http/1.1", nullptr};
};

QByteArray header(const http::Headers& headers, const char* name) {
    const auto value = http::GetHeader(headers, name);
    return value ? QByteArray(value->data(), static_cast<int>(value->size())) : QByteArray();
}

void addValidator(http::Headers& headers, const char* name, const QByteArray& value) {
    if (!value.isEmpty() && value.size() <= 1024 &&
            std::all_of(value.constBegin(), value.constEnd(), [](unsigned char c) {
                return c >= 32 && c < 127;
            })) {
        headers.emplace_back(name, std::string(value.constData(), value.size()));
    }
}

}  // namespace

QByteArray fetchOmmeshLinkPreview(const QByteArray& encodedUrl, const QAtomicInt* cancellation, int maximumBytes) {
    if (encodedUrl.isEmpty() || encodedUrl.size() > 2048 || cancelled(cancellation))
        return QByteArray();
    const auto deadline = absl::Now() + absl::Seconds(30);
    auto tcp = net::TcpClient::ConnectHost(kPreviewHost, 443, absl::Now() + absl::Seconds(8));
    if (!tcp.ok())
        return QByteArray();
    auto tls = std::make_unique<NativeTlsStream>(std::move(*tcp), cancellation);
    if (!tls->open(kPreviewHost, kPreviewRoots, sizeof(kPreviewRoots), deadline).ok())
        return QByteArray();

    http::RequestHead outbound;
    outbound.scheme = "https";
    outbound.authority = kPreviewHost;
    const QByteArray parameter = QUrl::toPercentEncoding(QString::fromLatin1(encodedUrl.constData(), encodedUrl.size()));
    outbound.path = "/v1/preview?url=" + std::string(parameter.constData(), parameter.size());
    outbound.headers = {{"user-agent", kUserAgent}, {"accept", "application/json"}, {"accept-encoding", "identity"}};
    http::Limits limits;
    limits.maximum_body_bytes = maximumBytes;
    auto exchange = http::Connection::Client(std::move(tls), std::move(outbound),
                                              http::Protocol::kHttp11, limits, 0, deadline);
    if (!exchange.ok() || !(*exchange)->Finish(deadline).ok() || !(*exchange)->ReceiveHeaders(deadline).ok())
        return QByteArray();
    const auto& response = (*exchange)->response();
    const auto encoding = header(response.headers, "content-encoding").trimmed().toLower();
    const auto type = header(response.headers, "content-type").split(';').first().trimmed().toLower();
    if (response.status != 200 || type != "application/json" || (!encoding.isEmpty() && encoding != "identity"))
        return QByteArray();

    QByteArray body;
    while (!cancelled(cancellation)) {
        const auto bytes = (*exchange)->Read(deadline);
        if (!bytes.ok())
            break;
        if (!bytes->has_value())
            return body;
        if ((**bytes).size() > static_cast<std::size_t>(maximumBytes - body.size()))
            break;
        body.append((**bytes).data(), static_cast<int>((**bytes).size()));
    }
    return QByteArray();
}

int transferAttachment(const QUrl& address, bool put, const QByteArray& contentType,
                       const QByteArray& disposition, const QByteArray& body,
                       const QByteArray& customCa, const QAtomicInt* cancellation) {
    const QByteArray host = QUrl::toAce(address.host()).toLower();
    const int port = address.port(443);
    if (address.scheme() != QLatin1String("https") || host.isEmpty() || port < 1 || port > 65535 ||
            !address.userName().isEmpty() || !address.password().isEmpty() ||
            address.hasFragment() || body.size() > 512 * 1024 || customCa.size() > 64 * 1024)
        return -1;
    if (cancelled(cancellation))
        return -4;
    const auto failureCode = [cancellation](const absl::Status& status) {
        if (cancelled(cancellation) || absl::IsCancelled(status))
            return -4;
        return absl::IsDeadlineExceeded(status) ? -3 : -1;
    };
    const auto deadline = absl::Now() + absl::Seconds(75);
    auto tcp = net::TcpClient::ConnectHost(host.constData(), static_cast<std::uint16_t>(port),
                                           absl::Now() + absl::Seconds(8));
    if (!tcp.ok())
        return failureCode(tcp.status());
    auto tls = std::make_unique<NativeTlsStream>(std::move(*tcp), cancellation);
    QByteArray roots(kAmazonRoots);
    roots.append(kPreviewRoots);
    roots.append(kTileRoots);
    roots.append('\n').append(customCa);
    auto status = tls->open(host.constData(), roots.constData(), roots.size() + 1, deadline);
    if (!status.ok()) {
        const int error = failureCode(status);
        return error == -1 ? -2 : error;
    }

    http::RequestHead outbound;
    outbound.method = put ? "PUT" : "GET";
    outbound.scheme = "https";
    outbound.authority = host.constData();
    if (port != 443)
        outbound.authority += ":" + std::to_string(port);
    const QByteArray path = address.encodedPath();
    outbound.path = path.isEmpty() ? "/" : std::string(path.constData(), path.size());
    if (address.hasQuery()) {
        const QByteArray query = address.encodedQuery();
        outbound.path += "?" + std::string(query.constData(), query.size());
    }
    outbound.headers = {{"user-agent", kUserAgent}, {"accept-encoding", "identity"}};
    if (put) {
        addValidator(outbound.headers, "content-type", contentType);
        addValidator(outbound.headers, "content-disposition", disposition);
    }
    http::Limits limits;
    limits.maximum_body_bytes = 512 * 1024;
    auto exchange = http::Connection::Client(std::move(tls), std::move(outbound),
        http::Protocol::kHttp11, limits, put ? static_cast<std::size_t>(body.size()) : 0, deadline);
    if (!exchange.ok())
        return failureCode(exchange.status());
    if (put) {
        for (int offset = 0; offset < body.size(); offset += 8192) {
            const int size = std::min(8192, body.size() - offset);
            status = (*exchange)->Write(std::string_view(body.constData() + offset, size), deadline);
            if (!status.ok())
                return failureCode(status);
        }
    }
    status = (*exchange)->Finish(deadline);
    if (status.ok())
        status = (*exchange)->ReceiveHeaders(deadline);
    return status.ok() ? (*exchange)->response().status : failureCode(status);
}
#endif
