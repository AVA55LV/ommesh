#include "native_tile_https.hpp"

#ifdef Q_OS_SYMBIAN
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
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

namespace {

namespace http = symbian::http;
namespace net = symbian::api::connectivity;

constexpr char kHost[] = "tile.openstreetmap.org";
constexpr char kPreviewHost[] = "preview.ommesh.ru";
constexpr char kUserAgent[] = "OMMesh-Belle-SIS/0.2.19 (+https://github.com/cm4ker/ommesh)";
constexpr int kMaximumTileBytes = 160 * 1024;
constexpr std::size_t kNativeIoLimit = 32 * 1024;
constexpr char kPng256Header[] =
    "\x89" "PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR"
    "\x00\x00\x01\x00\x00\x00\x01\x00";

bool cancelled(const QAtomicInt* flag) {
    return flag && const_cast<QAtomicInt*>(flag)->fetchAndAddAcquire(0) != 0;
}

absl::Status tlsError(const char* operation, int code) {
    return absl::UnavailableError(std::string(operation) + ": " + std::to_string(code));
}

class TileTlsStream final : public symbian::net::ByteStream {
public:
    TileTlsStream(net::TcpClient tcp, const QAtomicInt* cancellation)
        : tcp_(std::move(tcp)), cancelled_(cancellation) {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&config_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_x509_crt_init(&roots_);
    }

    ~TileTlsStream() override {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_x509_crt_free(&roots_);
    }

    absl::Status open(const char* host, const char* trustRoots, std::size_t trustRootsLength,
                      bool allowUntrustedCa, absl::Time deadline) {
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
        if (allowUntrustedCa)
            mbedtls_ssl_set_verify(&ssl_, verifyPresentedCa, this);
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
            return absl::DataLossError("Tile TLS stream closed without an authenticated end");
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
            return absl::CancelledError("Tile request cancelled");
        if (absl::Now() >= deadline_)
            return absl::DeadlineExceededError("Tile request timed out");
        return absl::OkStatus();
    }

    // Mbed TLS calls the verifier from the top of the presented chain down.
    // Only an intermediate/root CA may lack a trust anchor. A self-signed leaf,
    // hostname, lifetime, and signatures below that CA retain normal checks.
    static int verifyPresentedCa(void* context, mbedtls_x509_crt* cert, int depth,
                                 std::uint32_t* flags) {
        auto& self = *static_cast<TileTlsStream*>(context);
        if (std::exchange(self.firstVerification_, false) && depth > 0 &&
                cert->MBEDTLS_PRIVATE(ca_istrue))
            *flags &= ~static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_NOT_TRUSTED);
        return 0;
    }

    static int send(void* context, const unsigned char* bytes, std::size_t size) {
        auto& self = *static_cast<TileTlsStream*>(context);
        self.ioError_ = self.checkDeadline();
        if (self.ioError_.ok())
            self.ioError_ = self.tcp_.Send(std::span(bytes, size), self.deadline_);
        return self.ioError_.ok() ? static_cast<int>(size) : MBEDTLS_ERR_NET_SEND_FAILED;
    }

    static int receive(void* context, unsigned char* bytes, std::size_t size) {
        auto& self = *static_cast<TileTlsStream*>(context);
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
    bool firstVerification_ = true;
};

TileHttpsResult failure(const absl::Status& status) {
    TileHttpsResult result;
    result.error = QString::fromUtf8(status.ToString().c_str());
    return result;
}

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
#endif

TileHttpsResult fetchOsmTile(const TileHttpsRequest& request) {
#ifdef Q_OS_SYMBIAN
    if (request.z < 0 || request.z > 19 || request.x < 0 || request.y < 0 ||
            request.x >= (1 << request.z) || request.y >= (1 << request.z)) {
        return failure(absl::InvalidArgumentError("Invalid tile coordinates"));
    }
    if (cancelled(request.cancelled))
        return failure(absl::CancelledError("Tile request cancelled"));

    const auto deadline = absl::Now() + absl::Seconds(19);
    auto tcp = net::TcpClient::ConnectHost(kHost, 443, absl::Now() + absl::Seconds(8));
    if (!tcp.ok())
        return failure(tcp.status());
    auto tls = std::make_unique<TileTlsStream>(std::move(*tcp), request.cancelled);
    auto status = tls->open(kHost, kTileRoots, sizeof(kTileRoots), request.allowUntrustedCa, deadline);
    if (!status.ok())
        return failure(status);

    http::RequestHead outbound;
    outbound.scheme = "https";
    outbound.authority = kHost;
    outbound.path = "/" + std::to_string(request.z) + "/" + std::to_string(request.x) +
                    "/" + std::to_string(request.y) + ".png";
    outbound.headers = {
        {"user-agent", kUserAgent},
        {"accept", "image/png"},
        {"accept-encoding", "identity"},
    };
    const auto baseHeaderCount = outbound.headers.size();
    addValidator(outbound.headers, "if-none-match", request.etag);
    addValidator(outbound.headers, "if-modified-since", request.lastModified);
    const bool conditional = outbound.headers.size() > baseHeaderCount;
    http::Limits limits;
    limits.maximum_body_bytes = kMaximumTileBytes;
    auto exchange = http::Connection::Client(std::move(tls), std::move(outbound),
                                              http::Protocol::kHttp11, limits, 0, deadline);
    if (!exchange.ok())
        return failure(exchange.status());
    status = (*exchange)->Finish(deadline);
    if (status.ok())
        status = (*exchange)->ReceiveHeaders(deadline);
    if (!status.ok())
        return failure(status);

    const auto& response = (*exchange)->response();
    if (response.status == 200 && http::GetHeader(response.headers, "x-blocked"))
        return failure(absl::UnavailableError("OpenStreetMap declined this tile request"));
    if (response.status != 200 && (response.status != 304 || !conditional) &&
            (response.status < 400 || response.status > 599)) {
        return failure(absl::UnavailableError("Unexpected OpenStreetMap response " +
                                               std::to_string(response.status)));
    }

    TileHttpsResult result;
    result.status = response.status;
    result.contentType = header(response.headers, "content-type");
    result.cacheControl = header(response.headers, "cache-control");
    result.etag = header(response.headers, "etag");
    result.lastModified = header(response.headers, "last-modified");
    result.expires = header(response.headers, "expires");
    result.age = header(response.headers, "age");
    if (result.status != 200)
        return result;
    if (result.contentType.trimmed().toLower() != "image/png")
        return failure(absl::DataLossError("OpenStreetMap response is not a PNG tile"));
    const auto encoding = header(response.headers, "content-encoding").trimmed().toLower();
    if (!encoding.isEmpty() && encoding != "identity")
        return failure(absl::DataLossError("OpenStreetMap sent an unsupported content encoding"));

    while (!cancelled(request.cancelled)) {
        auto bytes = (*exchange)->Read(deadline);
        if (!bytes.ok())
            return failure(bytes.status());
        if (!bytes->has_value()) {
            if (!result.body.startsWith(QByteArray::fromRawData(kPng256Header, sizeof(kPng256Header) - 1)))
                return failure(absl::DataLossError("Invalid PNG tile signature or dimensions"));
            return result;
        }
        const auto size = (**bytes).size();
        if (size > static_cast<std::size_t>(kMaximumTileBytes - result.body.size()))
            return failure(absl::ResourceExhaustedError("OpenStreetMap tile exceeds size limit"));
        result.body.append((**bytes).data(), static_cast<int>(size));
    }
    return failure(absl::CancelledError("Tile request cancelled"));
#else
    Q_UNUSED(request);
    TileHttpsResult result;
    result.error = QString::fromLatin1("The native OpenStreetMap transport requires Symbian Belle");
    return result;
#endif
}

#ifdef Q_OS_SYMBIAN
QByteArray fetchOmmeshLinkPreview(const QByteArray& encodedUrl, const QAtomicInt* cancellation, int maximumBytes) {
    if (encodedUrl.isEmpty() || encodedUrl.size() > 2048 || cancelled(cancellation))
        return QByteArray();
    const auto deadline = absl::Now() + absl::Seconds(30);
    auto tcp = net::TcpClient::ConnectHost(kPreviewHost, 443, absl::Now() + absl::Seconds(8));
    if (!tcp.ok())
        return QByteArray();
    auto tls = std::make_unique<TileTlsStream>(std::move(*tcp), cancellation);
    if (!tls->open(kPreviewHost, kPreviewRoots, sizeof(kPreviewRoots), false, deadline).ok())
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
#endif
