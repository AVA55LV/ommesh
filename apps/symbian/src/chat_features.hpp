#ifndef OMMESH_BELLE_CHAT_FEATURES_HPP
#define OMMESH_BELLE_CHAT_FEATURES_HPP

#include "appearance.hpp"
#include "client.hpp"

#include <QtCore/QPoint>
#include <QtCore/QStringList>
#include <QtGui/QWidget>
#include <optional>

class QAction;
class QLabel;
class QPushButton;
class QTextEdit;
class ChatHistory;
namespace LinkPreview { class Store; }

namespace ChatFeatures {

struct ReplyQuote {
    QString author;
    QString excerpt;
    QString body;
    QString prefix;

    bool valid() const { return !prefix.isEmpty(); }
};

ReplyQuote splitQuote(const QString& text);
QString makeQuotePrefix(const QString& author, const QString& message,
                        const QStringList& newerTexts, int availableBytes);
int emojiCount();
QString emojiValue(int index);

} // namespace ChatFeatures

class ChatView : public QWidget {
    Q_OBJECT

public:
    explicit ChatView(QWidget* parent = 0);
    void setContext(const QString& title, const QString& status, const QString& activeId);
    void setMessages(const QVector<ChatMessage>& messages, bool opening = false);
    void setDraft(const QString& text);
    QString draft() const;
    void clearDraft();
    void followSentMessage(const QString& text);
    void setSendingState(bool canSend, int maxBytes);
    void setPinned(bool pinned);
    void syncLinkPreviewMode();
    void setAppearance(const Appearance::Palette& palette, int chatTextSize);

signals:
    void backRequested();
    void sendRequested(const QString& text);
    void resendRequested(const QString& text);
    void reactionRequested(const ChatMessage& message, const QString& emoji);
    void senderBlockRequested(const QString& id, const QString& name, bool blocked);
    void pinRequested();

private slots:
    void chooseEmoji();
    void removeQuote();
    void updateComposer();
    void send();
    void showMessageActions();
    void selectQuickReaction(int choice);
    void applyDeferredMessages();
    void refreshLinkPreviews();

private:
    friend class ChatHistory;

    QString composerText(int start = 0, int end = -1) const;
    QString outgoingText(const QString& body) const;
    void showQuotePreview();
    void queueMessageActions(int index, const QPoint& globalPosition, bool blockedRun = false);
    int pickEmoji(const QString& title);
    void reactTo(const ChatMessage& message, const QString& emoji);
    void replyTo(const ChatMessage& message, const QStringList& newerTexts);
    void showDetails(const ChatMessage& message);

    QLabel* title_;
    QLabel* status_;
    QLabel* byteLimit_;
    QAction* pinAction_;
    ChatHistory* history_;
    LinkPreview::Store* linkPreviews_ = nullptr;
    bool previewRefreshPending_ = false;
    QWidget* quotePreview_;
    QTextEdit* quoteText_;
    QTextEdit* composer_;
    QPushButton* emojiButton_;
    QPushButton* sendButton_;
    QString activeId_;
    QVector<ChatMessage> messages_;
    QVector<ChatMessage> deferredMessages_;
    std::optional<ChatMessage> unreadMarker_;
    QString pendingSentText_;
    quint32 pendingSentAfter_ = 0;
    bool hasDeferredMessages_ = false;
    ChatFeatures::ReplyQuote quote_;
    ChatMessage actionMessage_;
    QStringList actionNewerTexts_;
    QString actionTarget_;
    QPoint actionPosition_;
    bool actionBlockedRun_ = false;
    Appearance::Palette palette_;
    int chatTextSize_;
    int maxBytes_;
    int reactionChoice_;
    bool canSend_;
    bool editingDraft_;
    bool restoreDisplay_;
    bool hasMessageSnapshot_;
};

#endif // OMMESH_BELLE_CHAT_FEATURES_HPP
