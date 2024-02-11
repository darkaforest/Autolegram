package org.darkaforest.tdlib.autolegram;

import org.darkaforest.tdlib.Client;
import org.darkaforest.tdlib.TdApi;

import java.io.File;
import java.io.IOError;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.text.DecimalFormat;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.NavigableSet;
import java.util.Properties;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.stream.Collectors;

public final class Autolegram {
    private static final Lock AUTHORIZATION_LOCK = new ReentrantLock();

    private static final Condition GOT_AUTHORIZATION = AUTHORIZATION_LOCK.newCondition();

    private static final ConcurrentMap<Long, TdApi.User> USERS = new ConcurrentHashMap<>();

    private static final ConcurrentMap<Long, TdApi.Chat> CHATS = new ConcurrentHashMap<>();

    private static final NavigableSet<OrderedChat> MAIN_CHAT_LIST = new TreeSet<>();

    private static final Logger LOGGER = Logger.getLogger("hidden.logger.name");

    private static final Set<String> UNIQUE_ID_SET = new HashSet<>();

    private static final Map<Integer, Boolean> LOCAL_FILE_ACTIVE_MAP = new HashMap<>();

    private static final String CONFIG_FILE_PATH = "./conf/config.properties";

    private static final TxTQueue queue = new TxTQueue();

    private static final List<String> fileBotPrefix = List.of("vi_", "p_", "au_", "d_", "pk_");

    public static final int FILE_SIZE_B = 1024;

    public static final int FILE_SIZE_KB = 1048576;

    public static final int FILE_SIZE_MB = 1073741824;

    public static final int TIME_10 = 10;

    public static final int TIME_60 = 60;

    public static final int TIME_24 = 24;

    public static final String CHECKSUM_ALGORITHM = "MD5";

    public static final int FILE_READ_BUFFER_SIZE = 8 * 1024;

    private static Client client = null;

    private static boolean haveFullMainChatList = false;

    private static TdApi.AuthorizationState authorizationState = null;

    private static volatile boolean haveAuthorization = false;
    
    private static String proxyServer;

    private static int proxyPort;

    private static String proxyUsername;

    private static String proxyPassword;

    private static String proxyType;

    private static String proxySecret;

    private static boolean proxyHttpOnly;

    private static String telegramUserPhone;

    private static String getTelegramUserPassword;

    private static String telegramUserEmail;

    private static int telegramClientApiId;

    private static String telegramClientApiHash;

    private static String telegramClientLanguage;

    private static String telegramClientDeviceModel;

    private static String telegramClientVersion;

    private static String telegramDataPath;

    private static boolean telegramDataChecksum;

    private static boolean telegramDataEnable;

    private static boolean telegramDataShowDetail;

    private static long telegramChatFilesDriveId;

    private static int telegramMaxChatSize;

    public static String uniqueIdFilePath;

    private static String txtQueueFilePath;

    private static long lastFilesDriveUpdatedTime = 0;

    private static boolean filesDriveLoopStarted = false;

    private static Map<String, Integer> retryMap = new HashMap<>();

    private static class OrderedChat implements Comparable<OrderedChat> {
        final long chatId;

        final TdApi.ChatPosition position;

        OrderedChat(long chatId, TdApi.ChatPosition position) {
            this.chatId = chatId;
            this.position = position;
        }

        @Override
        public int compareTo(OrderedChat o) {
            if (this.position.order != o.position.order) {
                return o.position.order < this.position.order ? -1 : 1;
            }
            if (this.chatId != o.chatId) {
                return o.chatId < this.chatId ? -1 : 1;
            }
            return 0;
        }

        @Override
        public boolean equals(Object obj) {
            OrderedChat o = (OrderedChat) obj;
            return this.chatId == o.chatId && this.position.order == o.position.order;
        }
    }

    private static class UpdateHandler implements Client.ResultHandler {
        @Override
        public void onResult(TdApi.Object object) {
            switch (object.getConstructor()) {
                case TdApi.UpdateNewMessage.CONSTRUCTOR:
                    try {
                        onNewMessageUpdated((TdApi.UpdateNewMessage) object);
                    } catch (Throwable e) {
                        e.printStackTrace();
                    }
                    break;
                case TdApi.UpdateFile.CONSTRUCTOR:
                    onFileUpdated((TdApi.UpdateFile) object);
                    break;
                case TdApi.UpdateAuthorizationState.CONSTRUCTOR:
                    onAuthorizationStateUpdated(((TdApi.UpdateAuthorizationState) object).authorizationState);
                    break;
                case TdApi.UpdateUser.CONSTRUCTOR:
                    TdApi.UpdateUser updateUser = (TdApi.UpdateUser) object;
                    USERS.put(updateUser.user.id, updateUser.user);
                    break;
                case TdApi.UpdateUserStatus.CONSTRUCTOR: {
                    TdApi.UpdateUserStatus updateUserStatus = (TdApi.UpdateUserStatus) object;
                    TdApi.User user = USERS.get(updateUserStatus.userId);
                    synchronized (user) {
                        user.status = updateUserStatus.status;
                    }
                    break;
                }
                case TdApi.UpdateNewChat.CONSTRUCTOR: {
                    TdApi.UpdateNewChat updateNewChat = (TdApi.UpdateNewChat) object;
                    TdApi.Chat chat = updateNewChat.chat;
                    synchronized (chat) {
                        CHATS.put(chat.id, chat);

                        TdApi.ChatPosition[] positions = chat.positions;
                        chat.positions = new TdApi.ChatPosition[0];
                        setChatPositions(chat, positions);
                    }
                    break;
                }
                case TdApi.UpdateChatTitle.CONSTRUCTOR: {
                    TdApi.UpdateChatTitle updateChat = (TdApi.UpdateChatTitle) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.title = updateChat.title;
                    }
                    break;
                }
                case TdApi.UpdateChatPhoto.CONSTRUCTOR: {
                    TdApi.UpdateChatPhoto updateChat = (TdApi.UpdateChatPhoto) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.photo = updateChat.photo;
                    }
                    break;
                }
                case TdApi.UpdateChatLastMessage.CONSTRUCTOR: {
                    TdApi.UpdateChatLastMessage updateChat = (TdApi.UpdateChatLastMessage) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.lastMessage = updateChat.lastMessage;
                        setChatPositions(chat, updateChat.positions);
                    }
                    break;
                }
                case TdApi.UpdateChatPosition.CONSTRUCTOR: {
                    TdApi.UpdateChatPosition updateChat = (TdApi.UpdateChatPosition) object;
                    if (updateChat.position.list.getConstructor() != TdApi.ChatListMain.CONSTRUCTOR) {
                        break;
                    }
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        int i;
                        for (i = 0; i < chat.positions.length; i++) {
                            if (chat.positions[i].list.getConstructor() == TdApi.ChatListMain.CONSTRUCTOR) {
                                break;
                            }
                        }
                        TdApi.ChatPosition[] new_positions = new TdApi.ChatPosition[chat.positions.length
                                + (updateChat.position.order == 0 ? 0 : 1) - (i < chat.positions.length ? 1 : 0)];
                        int pos = 0;
                        if (updateChat.position.order != 0) {
                            new_positions[pos++] = updateChat.position;
                        }
                        for (int j = 0; j < chat.positions.length; j++) {
                            if (j != i) {
                                new_positions[pos++] = chat.positions[j];
                            }
                        }
                        setChatPositions(chat, new_positions);
                    }
                    break;
                }
                case TdApi.UpdateChatReadInbox.CONSTRUCTOR: {
                    TdApi.UpdateChatReadInbox updateChat = (TdApi.UpdateChatReadInbox) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.lastReadInboxMessageId = updateChat.lastReadInboxMessageId;
                        chat.unreadCount = updateChat.unreadCount;
                    }
                    break;
                }
                case TdApi.UpdateChatReadOutbox.CONSTRUCTOR: {
                    TdApi.UpdateChatReadOutbox updateChat = (TdApi.UpdateChatReadOutbox) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.lastReadOutboxMessageId = updateChat.lastReadOutboxMessageId;
                    }
                    break;
                }
                case TdApi.UpdateChatUnreadMentionCount.CONSTRUCTOR: {
                    TdApi.UpdateChatUnreadMentionCount updateChat = (TdApi.UpdateChatUnreadMentionCount) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.unreadMentionCount = updateChat.unreadMentionCount;
                    }
                    break;
                }
                case TdApi.UpdateMessageMentionRead.CONSTRUCTOR: {
                    TdApi.UpdateMessageMentionRead updateChat = (TdApi.UpdateMessageMentionRead) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.unreadMentionCount = updateChat.unreadMentionCount;
                    }
                    break;
                }
                case TdApi.UpdateChatReplyMarkup.CONSTRUCTOR: {
                    TdApi.UpdateChatReplyMarkup updateChat = (TdApi.UpdateChatReplyMarkup) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.replyMarkupMessageId = updateChat.replyMarkupMessageId;
                    }
                    break;
                }
                case TdApi.UpdateChatDraftMessage.CONSTRUCTOR: {
                    TdApi.UpdateChatDraftMessage updateChat = (TdApi.UpdateChatDraftMessage) object;
                    TdApi.Chat chat = CHATS.get(updateChat.chatId);
                    synchronized (chat) {
                        chat.draftMessage = updateChat.draftMessage;
                        setChatPositions(chat, updateChat.positions);
                    }
                    break;
                }
                case TdApi.UpdateChatPermissions.CONSTRUCTOR: {
                    TdApi.UpdateChatPermissions update = (TdApi.UpdateChatPermissions) object;
                    TdApi.Chat chat = CHATS.get(update.chatId);
                    synchronized (chat) {
                        chat.permissions = update.permissions;
                    }
                    break;
                }
                case TdApi.UpdateChatNotificationSettings.CONSTRUCTOR: {
                    TdApi.UpdateChatNotificationSettings update = (TdApi.UpdateChatNotificationSettings) object;
                    TdApi.Chat chat = CHATS.get(update.chatId);
                    synchronized (chat) {
                        chat.notificationSettings = update.notificationSettings;
                    }
                    break;
                }
                case TdApi.UpdateChatDefaultDisableNotification.CONSTRUCTOR: {
                    TdApi.UpdateChatDefaultDisableNotification update = (TdApi.UpdateChatDefaultDisableNotification) object;
                    TdApi.Chat chat = CHATS.get(update.chatId);
                    synchronized (chat) {
                        chat.defaultDisableNotification = update.defaultDisableNotification;
                    }
                    break;
                }
                case TdApi.UpdateChatIsMarkedAsUnread.CONSTRUCTOR: {
                    TdApi.UpdateChatIsMarkedAsUnread update = (TdApi.UpdateChatIsMarkedAsUnread) object;
                    TdApi.Chat chat = CHATS.get(update.chatId);
                    synchronized (chat) {
                        chat.isMarkedAsUnread = update.isMarkedAsUnread;
                    }
                    break;
                }
                case TdApi.UpdateChatIsBlocked.CONSTRUCTOR: {
                    TdApi.UpdateChatIsBlocked update = (TdApi.UpdateChatIsBlocked) object;
                    TdApi.Chat chat = CHATS.get(update.chatId);
                    synchronized (chat) {
                        chat.isBlocked = update.isBlocked;
                    }
                    break;
                }
                case TdApi.UpdateChatHasScheduledMessages.CONSTRUCTOR: {
                    TdApi.UpdateChatHasScheduledMessages update = (TdApi.UpdateChatHasScheduledMessages) object;
                    TdApi.Chat chat = CHATS.get(update.chatId);
                    synchronized (chat) {
                        chat.hasScheduledMessages = update.hasScheduledMessages;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    private static class AuthorizationRequestHandler implements Client.ResultHandler {
        @Override
        public void onResult(TdApi.Object object) {
            switch (object.getConstructor()) {
                case TdApi.Error.CONSTRUCTOR:
                    LOGGER.info("[login] receive an error:\n" + object);
                    LOGGER.info("[login] sleep 5s to avoid ip banned by too many requests");
                    try {
                        Thread.sleep(5000);
                    } catch (InterruptedException ignored) {
                    }
                    onAuthorizationStateUpdated(null); // repeat last action
                    break;
                case TdApi.Ok.CONSTRUCTOR:
                    // result is already received through UpdateAuthorizationState, nothing to do
                    break;
                default:
                    LOGGER.info("[login] receive wrong response from tdlib:\n" + object);
            }
        }
    }

    private static class LogMessageHandler implements Client.LogMessageHandler {
        @Override
        public void onLogMessage(int verbosityLevel, String message) {
            if (verbosityLevel == 0) {
                onFatalError(message);
                return;
            }
            LOGGER.info(message);
        }
    }

    private static void dealFileBotToken(String str) {
        if (!filesDriveLoopStarted) {
            return;
        }
        List<String> tokens = extractFileBotToken(str);
        if (tokens.isEmpty()) {
            return;
        }
        for (String token : tokens) {
            queue.offer(token);
            LOGGER.info("[filebot] [add] offer to queue: " + token + " ,current size: " + queue.size());
        }
    }

    private static void sendMessage(long chatId, String msg) {
        if (chatId == 0) {
            return;
        }
        LOGGER.info("[message] [sending] [" + chatId + "] " + msg);
        TdApi.InputMessageContent content = new TdApi.InputMessageText(new TdApi.FormattedText(msg, null), false, true);
        client.send(new TdApi.SendMessage(chatId, 0, 0, null, null, content), object -> {
            if (object instanceof TdApi.Error) {
                LOGGER.info("[message] [sending] failed, chatId=" + chatId + ", msg=" + msg);
            } else {
                long messageId = ((TdApi.Message) object).id;
                LOGGER.info("[message] [sent] messageId=" + messageId);
            }
        });
    }

    private static void onNewFilesDriveBotMessageUpdated(TdApi.Chat chat, long senderId, TdApi.Message message) {
        if (!filesDriveLoopStarted) {
            return;
        }
        if (chat.id == telegramChatFilesDriveId && senderId == telegramChatFilesDriveId) {
            lastFilesDriveUpdatedTime = System.currentTimeMillis();
            TdApi.Message lastMessage = chat.lastMessage;
            if (lastMessage == null) {
                return;
            }
            if (message.replyMarkup instanceof TdApi.ReplyMarkupInlineKeyboard) {
                TdApi.InlineKeyboardButton button = ((TdApi.ReplyMarkupInlineKeyboard)message.replyMarkup).rows[0][0];
                if (button.text == null) {
                    return;
                }
                if (!button.text.trim().equalsIgnoreCase("load more") && !button.text.trim().equalsIgnoreCase("加载更多")) {
                    return;
                }
                TdApi.InlineKeyboardButtonTypeCallback buttonType = (TdApi.InlineKeyboardButtonTypeCallback) button.type;
                client.send(new TdApi.GetCallbackQueryAnswer(chat.id, message.id, new TdApi.CallbackQueryPayloadData(buttonType.data)), object -> {
                });
                LOGGER.info("[message][file bot] load more message sent");
                return;
            }
            if (lastMessage.content instanceof TdApi.MessageText) {
                String reply = ((TdApi.MessageText) lastMessage.content).text.text;
                if (!reply.equals("/help") && !reply.equals(queue.peek())) {
                    LOGGER.info("[message][file bot] received but not equal, ignored");
                    return;
                }
                if (message.content instanceof TdApi.MessageText) {
                    String text = ((TdApi.MessageText) message.content).text.text;
                    if (text.contains("password") || text.contains("密码")) {
                        LOGGER.info("[message][file bot] received but need password, ignored");
                        queue.poll();
                        sendMessage(telegramChatFilesDriveId, "i don't have any password");
                        filebotKickStart();
                        return;
                    }
                }
                if (reply.startsWith("pk_")) {
                    LOGGER.info("[message][file bot] received pk reply, ignored");
                    return;
                }
                LOGGER.info("[message][file bot] received single file");
                queue.poll();
                filebotKickStart();
            }
        }
    }

    private static void onNewMessageUpdated(TdApi.UpdateNewMessage updateNewMessage) {
        TdApi.Message message = updateNewMessage.message;
        TdApi.MessageSender messageSender = message.senderId;
        long senderId = 0;
        String firstName = "";
        String lastName = "";
        String userName = "";
        String phoneNumber = "";
        boolean isAnonymous = false;
        if (messageSender instanceof TdApi.MessageSenderUser) {
            TdApi.MessageSenderUser messageSenderUser = (TdApi.MessageSenderUser) messageSender;
            TdApi.User senderUser = USERS.get(messageSenderUser.userId);
            firstName = senderUser.firstName;
            lastName = senderUser.lastName;
            userName = senderUser.username;
            phoneNumber = senderUser.phoneNumber;
            senderId = senderUser.id;
        }
        if (messageSender instanceof TdApi.MessageSenderChat) {
            TdApi.MessageSenderChat messageSenderChat = (TdApi.MessageSenderChat) messageSender;
            TdApi.Chat chat = CHATS.get(messageSenderChat.chatId);
            userName = chat.title;
            isAnonymous = true;
            senderId = messageSenderChat.chatId;
        }
        TdApi.Chat chat = CHATS.get(message.chatId);
        int time = message.date;
        SimpleDateFormat sdf = new SimpleDateFormat("MM-dd-yyyy HH:mm:ss");
        String sd = sdf.format(new Date(time * 1000L));
        TdApi.MessageContent messageContent = message.content;
        LOGGER.info("[message] " + (isAnonymous ? "[anonymous] " : "")  + sd + " " + firstName + " " + lastName
                + " (" + userName + ") [" + phoneNumber + "] - " + chat.title + ":");
        if (messageContent instanceof TdApi.MessageText) {
            TdApi.MessageText messageText = (TdApi.MessageText) messageContent;
            LOGGER.info("[message] " + messageText.text.text.replace("\n", " ")
                    .replace("\t", " ").replace("\r", " "));
            dealFileBotToken(messageText.text.text);
        } else if (messageContent instanceof TdApi.MessageVideo) {
            TdApi.MessageVideo messageVideo = (TdApi.MessageVideo) messageContent;
            String caption = messageVideo.caption.text;
            TdApi.Video video = messageVideo.video;
            String videoName = video.fileName;
            int videoDuration = video.duration;
            int videoWidth = video.width;
            int videoHeight = video.height;
            boolean supportStreaming = video.supportsStreaming;
            int localFileId = video.video.id;
            long remoteFileSize = video.video.size;
            String remoteFileId = video.video.remote.id;
            String remoteFileUniqueId = video.video.remote.uniqueId;
            if (caption != null && !caption.isEmpty()) {
                LOGGER.info("[message] [video] desc=" + caption.replace("\n", " ")
                        .replace("\t", " ").replace("\r", " "));
            }
            LOGGER.info("[message] [video] name=" + videoName + ", size=" + formatFileSize(remoteFileSize)
                    + ", duration=" + formatTime(videoDuration) + ", resolution=" + videoWidth + "x" + videoHeight
                    + ", supportStreaming=" + supportStreaming + ", id=" + remoteFileId + ", uniqueId=" + remoteFileUniqueId);
            download(localFileId, remoteFileUniqueId);
            dealFileBotToken(caption);
        } else if (messageContent instanceof TdApi.MessagePhoto) {
            TdApi.MessagePhoto messagePhoto = (TdApi.MessagePhoto) messageContent;
            String caption = messagePhoto.caption.text;
            TdApi.PhotoSize lastPhotoSize = messagePhoto.photo.sizes[messagePhoto.photo.sizes.length - 1];
            int localFileId = lastPhotoSize.photo.id;
            long remoteFileSize = lastPhotoSize.photo.size;
            String remoteFileId = lastPhotoSize.photo.remote.id;
            String remoteFileUniqueId = lastPhotoSize.photo.remote.uniqueId;
            if (caption != null && !caption.isEmpty()) {
                LOGGER.info("[message] [photo] desc=" + caption.replace("\n", " ")
                        .replace("\t", " ").replace("\r", " "));
            }
            LOGGER.info("[message] [photo] size=" + formatFileSize(remoteFileSize) + ", id=" + remoteFileId
                    + ", uniqueId=" + remoteFileUniqueId);
            download(localFileId, remoteFileUniqueId);
            dealFileBotToken(caption);
        } else if (messageContent instanceof TdApi.MessageAnimation) {
            TdApi.MessageAnimation messageAnimation = (TdApi.MessageAnimation) messageContent;
            String caption = messageAnimation.caption.text;
            TdApi.Animation animation = messageAnimation.animation;
            String gifName = animation.fileName;
            int gifDuration = animation.duration;
            int gifWidth = animation.width;
            int gifHeight = animation.height;
            int localFileId = messageAnimation.animation.animation.id;
            long remoteFileSize = animation.animation.size;
            String remoteFileId = animation.animation.remote.id;
            String remoteFileUniqueId = animation.animation.remote.uniqueId;
            if (caption != null && !caption.isEmpty()) {
                LOGGER.info("[message] [gif] desc=" + caption.replace("\n", " ")
                        .replace("\t", " ").replace("\r", " "));
            }
            LOGGER.info("[message] [gif] name=" + gifName + ", size=" + formatFileSize(remoteFileSize)
                    + ", duration=" + formatTime(gifDuration) + ", resolution=" + gifWidth + "x" + gifHeight
                    + ", id=" + remoteFileId + ", uniqueId=" + remoteFileUniqueId);
            download(localFileId, remoteFileUniqueId);
            dealFileBotToken(caption);
        } else if (messageContent instanceof TdApi.MessageDocument) {
            TdApi.MessageDocument messageDocument = (TdApi.MessageDocument) messageContent;
            String caption = messageDocument.caption.text;
            TdApi.Document document = messageDocument.document;
            String fileName = document.fileName;
            int localFileId = document.document.id;
            long remoteFileSize = document.document.size;
            String remoteFileId = document.document.remote.id;
            String remoteFileUniqueId = document.document.remote.uniqueId;
            if (caption != null && !caption.isEmpty()) {
                LOGGER.info("[message] [file] desc=" + caption.replace("\n", " ")
                        .replace("\t", " ").replace("\r", " "));
            }
            LOGGER.info("[message] [file] name=" + fileName + ", size=" + formatFileSize(remoteFileSize)
                    + ", id=" + remoteFileId + ", uniqueId=" + remoteFileUniqueId);
            download(localFileId, remoteFileUniqueId);
            dealFileBotToken(caption);
        } else if (messageContent instanceof TdApi.MessageSticker) {
            TdApi.MessageSticker messageSticker = (TdApi.MessageSticker) messageContent;
            boolean isPremium = messageSticker.isPremium;
            long setId = messageSticker.sticker.setId;
            long customEmojiId = messageSticker.sticker.customEmojiId;
            String emoji = messageSticker.sticker.emoji;
            LOGGER.info("[message] [sticker]" + (isPremium ? "[premium]" : "") + " emoji=" + emoji
                    + ", setId=" + setId + ", customEmojiId=" + customEmojiId);
        } else if (messageContent instanceof TdApi.MessageAnimatedEmoji) {
            TdApi.MessageAnimatedEmoji messageAnimatedEmoji = (TdApi.MessageAnimatedEmoji) messageContent;
            String emoji = messageAnimatedEmoji.emoji;
            long setId = messageAnimatedEmoji.animatedEmoji.sticker.setId;
            long customEmojiId = messageAnimatedEmoji.animatedEmoji.sticker.customEmojiId;
            LOGGER.info("[message] [sticker] emoji=" + emoji + ", setId=" + setId + ", customEmojiId=" + customEmojiId);
        } else if (messageContent instanceof TdApi.MessageChatJoinByLink) {
            LOGGER.info("[operation] user joined the group by link, group=" + chat.title + ", userid=" + userName + ", username=" + firstName + " " + lastName + ", userPhone=" + phoneNumber);
        } else if (messageContent instanceof TdApi.MessageChatJoinByRequest) {
            LOGGER.info("[operation] user joined the group by request, group=" + chat.title + ", userid=" + userName + ", username=" + firstName + " " + lastName + ", userPhone=" + phoneNumber);
        } else if (messageContent instanceof TdApi.MessageChatAddMembers) {
            TdApi.MessageChatAddMembers addMembers = (TdApi.MessageChatAddMembers) messageContent;
            long[] userIds = addMembers.memberUserIds;
            if (userIds != null && userIds.length != 0) {
                for (long userid : userIds) {
                    TdApi.User joinUser = USERS.get(userid);
                    if (joinUser == null) {
                        LOGGER.info("[operation] user has been added to the group, group=" + chat.title + ", userid=" + userid);
                    } else {
                        LOGGER.info("[operation] user has been added to the group, group=" + chat.title + ", userid=" + joinUser.username + ", username=" + joinUser.firstName + " " + joinUser.lastName + ", userPhone=" + joinUser.phoneNumber);
                    }
                }
            }
        } else if (messageContent instanceof TdApi.MessageChatDeleteMember) {
            TdApi.MessageChatDeleteMember deleteMember = (TdApi.MessageChatDeleteMember) messageContent;
            TdApi.User deleteUser = USERS.get(deleteMember.userId);
            if (deleteUser == null) {
                LOGGER.info("[operation] user has been removed from the group, group=" + chat.title + ", userid=" + deleteMember.userId);
            } else {
                LOGGER.info("[operation] user has been removed from the group, group=" + chat.title + ", userid=" + deleteUser.username + ", username=" + deleteUser.firstName + " " + deleteUser.lastName + ", userPhone=" + deleteUser.phoneNumber);
            }
        } else if (messageContent instanceof TdApi.MessageChatChangePhoto) {
            TdApi.MessageChatChangePhoto changePhoto = (TdApi.MessageChatChangePhoto) messageContent;
            TdApi.PhotoSize lastPhotoSize = changePhoto.photo.sizes[changePhoto.photo.sizes.length - 1];
            int localFileId = lastPhotoSize.photo.id;
            long remoteFileSize = lastPhotoSize.photo.size;
            String remoteFileId = lastPhotoSize.photo.remote.id;
            String remoteFileUniqueId = lastPhotoSize.photo.remote.uniqueId;
            LOGGER.info("[operation] group changed photo, group=" + chat.title + ", size=" + formatFileSize(remoteFileSize) + ", id=" + remoteFileId
                    + ", uniqueId=" + remoteFileUniqueId);
            download(localFileId, remoteFileUniqueId);
        } else if (messageContent instanceof TdApi.MessagePinMessage) {
            TdApi.MessagePinMessage pinMessage = (TdApi.MessagePinMessage) messageContent;
            LOGGER.info("[operation] message pinned in group, group=" + chat.title + ", messageId=" + pinMessage.messageId);
        } else if (messageContent instanceof TdApi.MessagePoll) {
            TdApi.MessagePoll pollMessage = (TdApi.MessagePoll) messageContent;
            TdApi.Poll poll = pollMessage.poll;
            LOGGER.info("[operation] poll update, question=" + poll.question
                    + ((poll.options != null && poll.options.length != 0)
                    ? ", options=[" + Arrays.stream(poll.options).map(o -> o.text).collect(Collectors.joining(", ")) + "]"
                    : ""));
        } else {
            LOGGER.info("[message] [unsupported] " + messageContent.getClass().getSimpleName());
        }
        onNewFilesDriveBotMessageUpdated(chat, senderId, message);
    }

    private static void filebotKickStart() {
        if (queue.isEmpty()) {
            LOGGER.info("[message] [file bot] queue is empty");
            return;
        }
        String token = queue.peek();
        LOGGER.info("[message] [file bot] start to deal with " + token);
        sendMessage(telegramChatFilesDriveId, token);
    }

    private static void onAuthorizationStateUpdated(TdApi.AuthorizationState authorizationState) {
        if (authorizationState != null) {
            Autolegram.authorizationState = authorizationState;
        }
        switch (Autolegram.authorizationState.getConstructor()) {
            case TdApi.AuthorizationStateWaitTdlibParameters.CONSTRUCTOR:
                LOGGER.info("[login] setup client params");
                TdApi.SetTdlibParameters request = new TdApi.SetTdlibParameters();
                request.databaseDirectory = "tdlib";
                request.useMessageDatabase = true;
                request.useSecretChats = true;
                request.enableStorageOptimizer = true;
                request.apiId = telegramClientApiId;
                request.apiHash = telegramClientApiHash;
                request.systemLanguageCode = telegramClientLanguage;
                request.deviceModel = telegramClientDeviceModel;
                request.applicationVersion = telegramClientVersion;
                client.send(request, new AuthorizationRequestHandler());
                break;
            case TdApi.AuthorizationStateWaitPhoneNumber.CONSTRUCTOR: {
                String phoneNumber = telegramUserPhone;
                if (telegramUserPhone.isEmpty()) {
                    LOGGER.severe("[login] no user phone number has been set, exit!");
                    LOGGER.severe("[login] note, if you are not receiving a new auth code, then the auth code is same as previous");
                    System.exit(1);
                }
                LOGGER.info("[login] login user with phone number: " + phoneNumber);
                client.send(new TdApi.SetAuthenticationPhoneNumber(phoneNumber, null), new AuthorizationRequestHandler());
                break;
            }
            case TdApi.AuthorizationStateWaitOtherDeviceConfirmation.CONSTRUCTOR: {
                String link = ((TdApi.AuthorizationStateWaitOtherDeviceConfirmation) Autolegram.authorizationState).link;
                LOGGER.info("[login] confirm this login link on another device asap: " + link);
                break;
            }
            case TdApi.AuthorizationStateWaitEmailAddress.CONSTRUCTOR: {
                String emailAddress = telegramUserEmail;
                LOGGER.info("[login] login user with email: " + emailAddress);
                client.send(new TdApi.SetAuthenticationEmailAddress(emailAddress), new AuthorizationRequestHandler());
                break;
            }
            case TdApi.AuthorizationStateWaitEmailCode.CONSTRUCTOR: {
                LOGGER.info("[login] telegram has just sent you an authentication code to your email address");
                LOGGER.info("[login] need set email auth code in config file ASAP, it will be immediately auto reload and used for login!");
                LOGGER.info("[login] need set email auth code in config file ASAP, it will be immediately auto reload and used for login!");
                LOGGER.info("[login] need set email auth code in config file ASAP, it will be immediately auto reload and used for login!");
                String code = refreshEmailCode();
                while (code.isEmpty()) {
                    LOGGER.info("[login] hurry up! set auth code in config file and save it!");
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        throw new RuntimeException(e);
                    }
                    code = refreshEmailCode();
                }
                LOGGER.info("[login] read email code=" + code);
                client.send(new TdApi.CheckAuthenticationEmailCode(new TdApi.EmailAddressAuthenticationCode(code)), new AuthorizationRequestHandler());
                break;
            }
            case TdApi.AuthorizationStateWaitCode.CONSTRUCTOR: {
                LOGGER.info("[login] telegram has just sent you an authentication code via sms or telegram message");
                LOGGER.info("[login] need set auth code in config file ASAP, it will be immediately auto reload and used for login!");
                LOGGER.info("[login] need set auth code in config file ASAP, it will be immediately auto reload and used for login!");
                LOGGER.info("[login] need set auth code in config file ASAP, it will be immediately auto reload and used for login!");
                String code = refreshCode();
                while (code.isEmpty()) {
                    LOGGER.info("[login] hurry up! set auth code in config file and save it!");
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        throw new RuntimeException(e);
                    }
                    code = refreshCode();
                }
                LOGGER.info("[login] read code=" + code);
                client.send(new TdApi.CheckAuthenticationCode(code), new AuthorizationRequestHandler());
                break;
            }
            case TdApi.AuthorizationStateWaitRegistration.CONSTRUCTOR: {
                LOGGER.info("[login] user not found");
                break;
            }
            case TdApi.AuthorizationStateWaitPassword.CONSTRUCTOR: {
                LOGGER.info("[login] your telegram account needs a 2FA password, it should be set in config file");
                if (getTelegramUserPassword.isEmpty()) {
                    LOGGER.severe("[login] your telegram account needs a 2FA password, which has not been set yet, exit!");
                    LOGGER.severe("[login] note, if you are not receiving a new auth code, then the auth code is same as previous");
                }
                client.send(new TdApi.CheckAuthenticationPassword(getTelegramUserPassword), new AuthorizationRequestHandler());
                break;
            }
            case TdApi.AuthorizationStateReady.CONSTRUCTOR:
                LOGGER.info("[login] telegram account login successfully");
                haveAuthorization = true;
                AUTHORIZATION_LOCK.lock();
                try {
                    GOT_AUTHORIZATION.signal();
                } finally {
                    AUTHORIZATION_LOCK.unlock();
                }
                break;
            case TdApi.AuthorizationStateLoggingOut.CONSTRUCTOR:
                LOGGER.info("[logout] telegram account is logging out");
                haveAuthorization = false;
                break;
            case TdApi.AuthorizationStateClosing.CONSTRUCTOR:
                LOGGER.info("[logout] tdlib is closing");
                haveAuthorization = false;
                break;
            case TdApi.AuthorizationStateClosed.CONSTRUCTOR:
                LOGGER.info("[logout] telegram client quit");
                haveAuthorization = false;
                break;
            default:
                LOGGER.info("[login] Unsupported authorization state:\n" + Autolegram.authorizationState);
                break;
        }
    }

    private static void onFileUpdated(TdApi.UpdateFile updateFile) {
        TdApi.File file = updateFile.file;
        double fileSize = updateFile.file.size;
        double currSize = updateFile.file.local.downloadedSize;
        if (telegramDataShowDetail) {
            String progress = new DecimalFormat("0.00%").format(currSize / fileSize);
            LOGGER.info("[download] [async] file status update, localId=" + file.id + ", progress=" + progress
                    + ", activated=" + file.local.isDownloadingActive + ", completed=" + file.local.isDownloadingCompleted);
        } else {
            if (!LOCAL_FILE_ACTIVE_MAP.getOrDefault(file.id, false)
                    && file.local.isDownloadingActive) {
                LOGGER.info("[download] [async] download activated, localId=" + file.id);
                LOCAL_FILE_ACTIVE_MAP.put(file.id, true);
            }
            if (LOCAL_FILE_ACTIVE_MAP.getOrDefault(file.id, false)
                    && !file.local.isDownloadingActive
                    && !file.local.isDownloadingCompleted) {
                LOGGER.info("[download] [async] download deactivated, localId=" + file.id);
                LOCAL_FILE_ACTIVE_MAP.remove(file.id);
            }
            if (file.local.isDownloadingCompleted) {
                LOGGER.info("[download] [async] download competed, localId=" + file.id);
                LOCAL_FILE_ACTIVE_MAP.remove(file.id);
            }
        }
        if (currSize != fileSize) {
            return;
        }
        String localPath = file.local.path;
        File srcFile = new File(localPath);
        String outputPath = getDateSubDirName(srcFile.lastModified());
        createDir(outputPath);
        outputPath = outputPath + File.separator + "[" + file.remote.uniqueId + "]" + srcFile.getName();
        try {
            LOGGER.info("[download] [sync] file saved, copy to output dir, localId="
                    + file.id + ", localPath=" + localPath + ", outputPath=" + outputPath);
            Files.copy(Paths.get(localPath), Paths.get(outputPath));
            LOGGER.info("[download] [sync] copy finish");
        } catch (Exception e) {
            LOGGER.log(Level.SEVERE, "[download] [sync] copy file failed" + e);
        }
        File outFile = new File(outputPath);
        if (!outFile.exists()) {
            LOGGER.severe("[download] [sync] output file not found, delete src file");
            if (srcFile.delete()) {
                LOGGER.info("[download] [sync] src file deleted");
            } else {
                LOGGER.info("[download] [sync] src file delete failed");
            }
            UNIQUE_ID_SET.remove(file.remote.uniqueId);
            return;
        }
        String idPath = telegramDataPath + "donotdelete" + File.separator + "uniqueIds.txt";
        String uniqueId = file.remote.uniqueId;
        if (!telegramDataChecksum) {
            LOGGER.info("[download] [sync] checksum has been disabled");
            LOGGER.info("[download] [sync] download succeed, filename=" + outFile.getName());
            LOGGER.info("[download] [sync] save uniqueId to file for deduplication");
            if(appendLine(uniqueId)) {
                UNIQUE_ID_SET.add(uniqueId);
                LOGGER.info("[download] [sync] uniqueId saved, value=" + uniqueId);
            } else {
                LOGGER.info("[download] [sync] uniqueId save failed, may cause hard drive space waste, value=" + uniqueId);
            }
            if (srcFile.delete()) {
                LOGGER.info("[download] [sync] src file deleted");
            } else {
                LOGGER.info("[download] [sync] src file delete failed");
            }
            return;
        }
        LOGGER.info("[download] [sync] calculating checksum, could disable it in config if got any performance issue");
        String srcMD5 = md5(localPath);
        String dstMD5 = md5(outputPath);
        if (!srcMD5.equals(dstMD5)) {
            LOGGER.info("[download] [sync] download failed, md5 doesn't match, srcMD5=" + srcMD5 + ", dstMD5=" + dstMD5);
            if (srcFile.delete() && outFile.delete()) {
                LOGGER.info("[download] [sync] corrupted files deleted");
            } else {
                LOGGER.info("[download] [sync] try to delete corrupted files but failed");
            }
            UNIQUE_ID_SET.remove(file.remote.uniqueId);
            LOGGER.info("[download] [sync] uniqueId removed");
        } else {
            LOGGER.info("[download] [sync] download succeed, filename=" + outFile.getName() + ", md5=" + dstMD5);
            if (srcFile.delete()) {
                LOGGER.info("[download] [sync] src file deleted");
            } else {
                LOGGER.info("[download] [sync] src file delete failed");
            }
            if(appendLine(uniqueId)) {
                UNIQUE_ID_SET.add(uniqueId);
                LOGGER.info("[download] [sync] uniqueId saved, value=" + uniqueId);
            } else {
                LOGGER.info("[download] [sync] uniqueId save failed, may cause hard drive space waste, value=" + uniqueId);
            }
        }
    }

    private static void onFatalError(String errorMessage) {
        final class ThrowError implements Runnable {
            private final String errorMessage;

            private final AtomicLong errorThrowTime;

            private ThrowError(String errorMessage, AtomicLong errorThrowTime) {
                this.errorMessage = errorMessage;
                this.errorThrowTime = errorThrowTime;
            }

            @Override
            public void run() {
                if (isDatabaseBrokenError(errorMessage) || isDiskFullError(errorMessage) || isDiskError(errorMessage)) {
                    processExternalError();
                    return;
                }
                errorThrowTime.set(System.currentTimeMillis());
                LOGGER.severe("[client] " + errorMessage);
                throw new ClientError("TDLib fatal error: " + errorMessage);
            }

            private void processExternalError() {
                errorThrowTime.set(System.currentTimeMillis());
                LOGGER.severe("[client] " + errorMessage);
                throw new ExternalClientError("Fatal error: " + errorMessage);
            }

            final class ClientError extends Error {
                private ClientError(String message) {
                    super(message);
                }
            }

            final class ExternalClientError extends Error {
                public ExternalClientError(String message) {
                    super(message);
                }
            }

            private boolean isDatabaseBrokenError(String message) {
                return message.contains("Wrong key or database is corrupted") ||
                        message.contains("SQL logic error or missing database") ||
                        message.contains("database disk image is malformed") ||
                        message.contains("file is encrypted or is not a database") ||
                        message.contains("unsupported file format") ||
                        message.contains("Database was corrupted and deleted during execution and can't be recreated");
            }

            private boolean isDiskFullError(String message) {
                return message.contains("PosixError : No space left on device") ||
                        message.contains("database or disk is full");
            }

            private boolean isDiskError(String message) {
                return message.contains("I/O error") || message.contains("Structure needs cleaning");
            }
        }
        final AtomicLong errorThrowTime = new AtomicLong(Long.MAX_VALUE);
        new Thread(new ThrowError(errorMessage, errorThrowTime), "TDLib fatal error thread").start();
        // wait at least 10 seconds after the error is thrown
        while (errorThrowTime.get() >= System.currentTimeMillis() - 10000) {
            try {
                Thread.sleep(1000 /* milliseconds */);
            } catch (InterruptedException ignore) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private static void setChatPositions(TdApi.Chat chat, TdApi.ChatPosition[] positions) {
        synchronized (MAIN_CHAT_LIST) {
            synchronized (chat) {
                for (TdApi.ChatPosition position : chat.positions) {
                    if (position.list.getConstructor() == TdApi.ChatListMain.CONSTRUCTOR) {
                        boolean isRemoved = MAIN_CHAT_LIST.remove(new OrderedChat(chat.id, position));
                        assert isRemoved;
                    }
                }
                chat.positions = positions;
                for (TdApi.ChatPosition position : chat.positions) {
                    if (position.list.getConstructor() == TdApi.ChatListMain.CONSTRUCTOR) {
                        boolean isAdded = MAIN_CHAT_LIST.add(new OrderedChat(chat.id, position));
                        assert isAdded;
                    }
                }
            }
        }
    }

    public static void getMainChatList() {
        synchronized (MAIN_CHAT_LIST) {
            if (!haveFullMainChatList && telegramMaxChatSize > MAIN_CHAT_LIST.size()) {
                // send LoadChats request if there are some unknown chats and have not enough known chats
                client.send(new TdApi.LoadChats(new TdApi.ChatListMain(), telegramMaxChatSize - MAIN_CHAT_LIST.size()), object -> {
                    switch (object.getConstructor()) {
                        case TdApi.Error.CONSTRUCTOR:
                            if (((TdApi.Error) object).code == 404) {
                                synchronized (MAIN_CHAT_LIST) {
                                    haveFullMainChatList = true;
                                    getMainChatList();
                                }
                            } else {
                                LOGGER.info("[chat] receive an error for loadChats:\n" + object);
                            }
                            break;
                        case TdApi.Ok.CONSTRUCTOR:
                            // chats had already been received through updates, let's retry request
                            getMainChatList();
                            break;
                        default:
                            LOGGER.info("[chat] receive wrong response from tdlib:\n" + object);
                    }
                });
                return;
            }
            java.util.Iterator<OrderedChat> iter = MAIN_CHAT_LIST.iterator();
            LOGGER.info("[chat] first " + telegramMaxChatSize + " chat(s) out of " + MAIN_CHAT_LIST.size() + " known chat(s):");
            for (int i = 0; i < telegramMaxChatSize && i < MAIN_CHAT_LIST.size(); i++) {
                long chatId = iter.next().chatId;
                TdApi.Chat chat = CHATS.get(chatId);
                synchronized (chat) {
                    LOGGER.info("[chat] chatId=" + chatId + ", chatName=" + chat.title + ", unReadCount=" + chat.unreadCount);
                }
            }
        }
    }

    private static String formatFileSize(long size) {
        DecimalFormat df = new DecimalFormat("#.00");
        String fileSizeString = "";
        String wrongSize = "0B";
        if (size == 0) {
            return wrongSize;
        }
        if (size < FILE_SIZE_B) {
            fileSizeString = df.format((double) size) + "B";
        } else if (size < FILE_SIZE_KB) {
            fileSizeString = df.format((double) size / FILE_SIZE_B) + "KB";
        } else if (size < FILE_SIZE_MB) {
            fileSizeString = df.format((double) size / FILE_SIZE_KB) + "MB";
        } else {
            fileSizeString = df.format((double) size / FILE_SIZE_MB) + "GB";
        }
        return fileSizeString;
    }

    private static String formatTime(int second) {
        if (second < TIME_10) {
            return "00:0" + second;
        }
        if (second < TIME_60) {
            return "00:" + second;
        }
        int minutes = second / TIME_60;
        second %= TIME_60;
        int hours = minutes / TIME_60;
        minutes %= TIME_60;
        int day = hours / TIME_24;
        hours %= TIME_24;
        String secStr =  second < TIME_10 ? "0" + second : "" + second;
        String minStr = minutes < TIME_10 ? "0" + minutes : "" + minutes;
        String hourStr = hours < TIME_10 ? "0" + hours : "" + hours;
        return (day == 0 ? "" : day + ":") + (hours == 0 ? "" : hourStr + ":") + minStr + ":" + secStr;
    }

    private static Set<String> rebuildUniqueIdList() {
        List<File> fileList = new ArrayList<>();
        findAllFile(telegramDataPath, fileList);
        Set<String> uniqueIds = new HashSet<>();
        for (File file : fileList) {
            String filename = file.getName();
            if (file.isFile() && filename.startsWith("[") && filename.contains("]")) {
                String uniqueId = filename.substring(1, filename.indexOf("]"));
                uniqueIds.add(uniqueId.trim());
            }
        }
        return uniqueIds;
    }

    private static void reSortExistsOldFiles() {
        File rootDir = new File(telegramDataPath);
        File[] files = rootDir.listFiles();
        if (!rootDir.exists() || files == null) {
            return;
        }
        for (File file : files) {
            if (file == null || file.isDirectory()) {
                continue;
            }
            String dirName = getDateSubDirName(file.lastModified());
            try {
                createDir(dirName);
                Path dstFile = Paths.get(dirName + File.separator + file.getName());
                LOGGER.info("move old file " + file.getAbsolutePath() + " to " + dstFile);
                Files.move(Paths.get(file.getAbsolutePath()), dstFile);
            } catch (Exception e) {
                LOGGER.log(Level.SEVERE, "move file failed", e);
            }
        }
    }

    private static String getDateSubDirName(long millions) {
        SimpleDateFormat sdf = new SimpleDateFormat("MM-dd-yyyy");
        return telegramDataPath + sdf.format(new Date(millions));
    }

    private static void createDir(String dir) {
        try {
            Path path = Paths.get(dir);
            if (Files.notExists(path)) {
                LOGGER.info("create dir " + dir);
                Files.createDirectories(path);
            }
        } catch (Exception e) {
            LOGGER.log(Level.SEVERE, "create dir failed", e);
        }
    }

    private static void findAllFile(String path, List<File> fileList) {
        File rootDir = new File(path);
        File[] files = rootDir.listFiles();
        if (!rootDir.exists() || files == null) {
            return;
        }
        for (File file : files) {
            if (file == null) {
                continue;
            }
            fileList.add(file);
            if (file.isDirectory()) {
                findAllFile(file.getAbsolutePath(), fileList);
            }
        }
    }

    private static String md5(String filepath) {
        File file = new File(filepath);
        if (!file.exists()) {
            return "";
        }
        MessageDigest md;
        try {
            md = MessageDigest.getInstance(CHECKSUM_ALGORITHM);
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException(e);
        }
        try (InputStream is = Files.newInputStream(Paths.get(filepath));
             DigestInputStream dis = new DigestInputStream(is, md))
        {
            byte[] buffer = new byte[FILE_READ_BUFFER_SIZE];
            while (dis.read(buffer) > 0) {
            }
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
        byte[] digest = md.digest();
        StringBuilder sb = new StringBuilder();
        for (byte b : digest) {
            sb.append(String.format("%02x", b));
        }
        return sb.toString();
    }

    private static boolean appendLine(String line) {
        try {
            createDir(new File(uniqueIdFilePath).getParent());
            createDir(uniqueIdFilePath);
            Files.write(Paths.get(uniqueIdFilePath), (line + "\n").getBytes(), StandardOpenOption.APPEND);
        } catch (Exception e) {
            LOGGER.log(Level.SEVERE, "append file failed", e);
            return false;
        }
        return true;
    }

    private static List<String> extractFileBotToken(String msg) {
        List<String> list = new ArrayList<>();
        if (msg == null || msg.isEmpty()) {
            return list;
        }
        String plainMsg = msg.replace("\r\n", " ")
                .replace("\r", " ")
                .replace("\n", " ")
                .replace("\t", " ");
        String[] segments = plainMsg.split(" ");
        for (String segment : segments) {
            if (segment == null || segment.isEmpty()) {
                continue;
            }
            for (String prefix : fileBotPrefix) {
                if (segment.startsWith(prefix)) {
                    list.add(segment.trim());
                    break;
                }
            }
        }
        return list;
    }

    private static void rebuildUniqueIdTxt() {
        try {
            createDir(new File(uniqueIdFilePath).getParent());
            Path path = Paths.get(uniqueIdFilePath);
            if (Files.exists(path)) {
                List<String> uniqueIds = Files.readAllLines(path);
                if (!uniqueIds.isEmpty()) {
                    UNIQUE_ID_SET.addAll(uniqueIds);
                }
                Files.delete(path);
            }
            Files.createFile(path);
            StringBuilder sb = new StringBuilder();
            for (String uniqueId : UNIQUE_ID_SET) {
                sb.append(uniqueId).append("\n");
            }
            if (sb.length() != 0) {
                Files.write(path, sb.toString().getBytes(), StandardOpenOption.APPEND);
            }
        } catch (Exception e) {
            LOGGER.log(Level.SEVERE, "rebuild uniqueId.txt failed", e);
        }
    }

    private static void download(int localFileId, String uniqueId) {
        if (!telegramDataEnable) {
            return;
        }
        if (UNIQUE_ID_SET.contains(uniqueId)) {
            LOGGER.info("[download] [sync] file has already been saved, skip");
            return;
        }
        LOGGER.info("[download] [sync] send command to download file");
        TdApi.DownloadFile downloadFile = new TdApi.DownloadFile();
        downloadFile.fileId = localFileId;
        downloadFile.priority = 1;
        client.send(downloadFile, object -> {
            if (object instanceof TdApi.Error) {
                LOGGER.info("[download] [async] failed, " + object);
            } else if (object instanceof TdApi.File) {
                LOGGER.info("[download] [async] task created, localId=" + ((TdApi.File) object).id);
            }
        });
        LOGGER.info("[download] [sync] command sent");
    }

    private static boolean initFileSystem() {
        Path rootPath = Paths.get(telegramDataPath);
        if (Files.notExists(rootPath)) {
            LOGGER.info("[init] output path not exists, start create, value=" + telegramDataPath);
            try {
                Files.createDirectories(rootPath);
            } catch (Exception e) {
                LOGGER.log(Level.SEVERE, "[init] create output path failed, this is fatal", e);
                return false;
            }
        }
        reSortExistsOldFiles();
        UNIQUE_ID_SET.clear();
        UNIQUE_ID_SET.addAll(rebuildUniqueIdList());
        rebuildUniqueIdTxt();
        return true;
    }

    private static void startMsgLoop() throws InterruptedException {
        LOGGER.info("[init] main message loop active!");
        // main loop
        new Thread(() -> {
            try {
                while (true) {
                    Thread.sleep(1000 * 60 * 10);
                }
            } catch (InterruptedException ignored) {
            }
        }).start();
        // await authorization
        LOGGER.info("[init] wait user login");
        AUTHORIZATION_LOCK.lock();
        try {
            while (!haveAuthorization) {
                GOT_AUTHORIZATION.await();
            }
        } finally {
            AUTHORIZATION_LOCK.unlock();
        }
        if (haveAuthorization) {
            getMainChatList();
        }
        LOGGER.info("[init] main message loop started!");
    }

    private static void createClient() {
        LOGGER.info("[init] creating client start");
        // create client
        client = Client.create(new UpdateHandler(), null, null);
        LOGGER.info("[init] creating client finish");
    }

    private static void setUpTdlib() {
        LOGGER.info("[init] setting tdlib start");
        // set log message handler to handle only fatal errors (0) and plain log messages (-1)
        Client.setLogMessageHandler(0, new LogMessageHandler());
        // disable TDLib log and redirect fatal errors and plain log messages to a file
        Client.execute(new TdApi.SetLogVerbosityLevel(0));
        if (Client.execute(new TdApi.SetLogStream(new TdApi.LogStreamFile(
                "tdlib.log", 1 << 27, false))) instanceof TdApi.Error) {
            throw new IOError(new IOException("[init] Write access to the current directory is required"));
        }
        LOGGER.info("[init] setting tdlib finish");
    }

    private static void setUpProxyServer() {
        LOGGER.info("[init] setting proxy server start");
        if (proxyServer.isEmpty() || proxyPort == 0) {
            LOGGER.info("[init] no proxy server found, finish");
            return;
        }
        if (proxyType.isEmpty()) {
            LOGGER.info("[init] no proxy type found, finish");
            return;
        }
        TdApi.ProxyType proxyParams;
        switch (proxyType) {
            case "HTTP": {
                LOGGER.info("[init] proxy server type HTTP found");
                proxyParams = new TdApi.ProxyTypeHttp(proxyUsername, proxyPassword, proxyHttpOnly);
                break;
            }
            case "SOCKS5": {
                LOGGER.info("[init] proxy server type SOCKS5 found");
                proxyParams = new TdApi.ProxyTypeSocks5(proxyUsername, proxyPassword);
                break;
            }
            case "MTPROTO":
                LOGGER.info("[init] proxy server type MTPROTO found");
                proxyParams = new TdApi.ProxyTypeMtproto(proxySecret);
                break;
            default:
                LOGGER.info("[init] proxy type invalid, finish");
                return;
        }
        LOGGER.info("[init] [sync] send command to set proxy server");
        client.send(new TdApi.AddProxy(proxyServer, proxyPort, true, proxyParams),
                object -> LOGGER.info("[init] [async] add proxy result: " + object));
        LOGGER.info("[init] command sent");
    }

    private static void loadConfig() {
        LOGGER.info("[init] loading config start");
        try {
            Properties properties = new Properties();
            properties.load(Files.newInputStream(Paths.get(CONFIG_FILE_PATH)));
            proxyServer = properties.getProperty("proxy.server", "");
            String proxyPortString = properties.getProperty("proxy.port", "0");
            proxyPort = Integer.parseInt(proxyPortString.isEmpty() ? "0" : proxyPortString);
            proxyUsername = properties.getProperty("proxy.username", "");
            proxyPassword = properties.getProperty("proxy.password", "");
            proxyType = properties.getProperty("proxy.type", "").toUpperCase(Locale.getDefault());
            String proxyHttpOnlyString = properties.getProperty("proxy.httponly", "false");
            proxyHttpOnly = Boolean.parseBoolean(proxyHttpOnlyString.isEmpty() ? "false" : proxyHttpOnlyString);
            proxySecret = properties.getProperty("proxy.password", "");
            telegramUserPhone = properties.getProperty("telegram.user.phone", "");
            getTelegramUserPassword = properties.getProperty("telegram.user.password", "");
            telegramUserEmail = properties.getProperty("telegram.user.email", "");
            String telegramClientApiIdString = properties.getProperty("telegram.client.apiid", "0");
            telegramClientApiId = Integer.parseInt(telegramClientApiIdString.isEmpty() ? "0" : telegramClientApiIdString);
            telegramClientApiHash = properties.getProperty("telegram.client.apihash", "");
            telegramClientLanguage = properties.getProperty("telegram.client.language", "en");
            telegramClientDeviceModel = properties.getProperty("telegram.client.devicemodel", "Desktop");
            telegramClientVersion = properties.getProperty("telegram.client.version", "1.0");
            telegramDataPath = properties.getProperty("telegram.data.path", "./");
            if (!telegramDataPath.endsWith(File.separator)) {
                telegramDataPath = telegramDataPath + File.separator;
            }
            uniqueIdFilePath = telegramDataPath + "donotdelete" + File.separator + "uniqueIds.txt";
            txtQueueFilePath = telegramDataPath + "donotdelete" + File.separator + "queue.txt";
            queue.init(txtQueueFilePath);
            telegramDataChecksum = Boolean.parseBoolean(properties.getProperty("telegram.data.checksum", "false"));
            telegramDataEnable = Boolean.parseBoolean(properties.getProperty("telegram.data.enable", "true"));
            telegramDataShowDetail = Boolean.parseBoolean(properties.getProperty("telegram.data.showdetail", "false"));
            telegramMaxChatSize = Integer.parseInt(properties.getProperty("telegram.data.maxchatsize", "9999"));
            telegramChatFilesDriveId = Long.parseLong(properties.getProperty("telegram.chat.filesdrive.id", "0"));
        } catch (IOException e) {
            LOGGER.log(Level.SEVERE, "[init] loading config file failed", e);
        }
        LOGGER.info("[init] loading config finish");
    }

    private static String refreshCode() {
        try {
            Properties properties = new Properties();
            properties.load(Files.newInputStream(Paths.get(CONFIG_FILE_PATH)));
            return properties.getProperty("telegram.user.code", "");
        } catch (IOException e) {
            return "";
        }
    }

    private static String refreshEmailCode() {
        try {
            Properties properties = new Properties();
            properties.load(Files.newInputStream(Paths.get(CONFIG_FILE_PATH)));
            return properties.getProperty("telegram.user.emailcode", "");
        } catch (IOException e) {
            return "";
        }
    }

    public static void main(String[] args) throws InterruptedException, IOException {
        Locale.setDefault(new Locale("en", "EN"));
        Thread.currentThread().setName("main");
        loadConfig();
        if (!initFileSystem()) {
            LOGGER.severe("[init] fatal error, exit");
            return;
        }
        setUpTdlib();
        createClient();
        setUpProxyServer();
        startMsgLoop();
        startFilesDriveBotLoop();
    }

    private static void startFilesDriveBotLoop() {
        try{
            new Thread(() -> {
                try {
                    LOGGER.info("[file bot] delay start");
                    Thread.sleep(1000 * 60);
                    LOGGER.info("[file bot] delay end");
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
                filesDriveLoopStarted = true;
                filebotKickStart();
                lastFilesDriveUpdatedTime = System.currentTimeMillis();
                while (true) {
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        throw new RuntimeException(e);
                    }
                    if (System.currentTimeMillis() - lastFilesDriveUpdatedTime > 1000 * 60 * 2 + (int) (1000 * 60 * Math.random())) {
                        if (!queue.isEmpty()) {
                            String token = queue.peek();
                            if (retryMap.getOrDefault(token, 0) > 10) {
                                LOGGER.info("[file bot] evict failed token after retries: " + token);
                                queue.poll();
                                retryMap.remove(token);
                                continue;
                            }
                            if (token.startsWith("pk_")) {
                                retryMap.put(token, Integer.MAX_VALUE);
                            } else {
                                retryMap.put(token, retryMap.getOrDefault(token, 0) + 1);
                                queue.offer(queue.poll());
                            }
                        }
                        filebotKickStart();
                        lastFilesDriveUpdatedTime = System.currentTimeMillis();
                    }
                }
            }).start();
        } catch (Throwable e) {
            e.printStackTrace();
        }
    }
}
