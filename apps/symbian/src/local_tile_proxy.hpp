#ifndef OMMESH_BELLE_LOCAL_TILE_PROXY_HPP
#define OMMESH_BELLE_LOCAL_TILE_PROXY_HPP

#include <QtCore/QString>

#include <memory>

class LocalTileProxy {
public:
    LocalTileProxy();
    ~LocalTileProxy();

    bool start(bool allowUntrustedCa, QString* error = nullptr);
    void stop();
    bool running() const;
    QString baseUrl() const;
    QString lastError() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
