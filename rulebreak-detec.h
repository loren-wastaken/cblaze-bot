#pragma once
#include <dpp/dpp.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>

// Which rule was broken -- used to pick the hardcoded reason string
enum class RuleViolation {
    NONE,
    RULE_1_RESPECT,      // profanity / slurs / basic harassment keywords
    RULE_2_ILLEGAL,      // piracy / malware links / doxxing patterns
    RULE_4_NSFW,         // NSFW keywords / links
    RULE_5_SPAM          // flooding, excessive mentions, invite spam
};

struct WarningRecord {
    int count = 0;
    std::vector<std::string> reasons; // history of why they were warned
};

struct BanRecord {
    dpp::snowflake guild_id;
    dpp::snowflake user_id;
    std::chrono::system_clock::time_point unban_at;
};

class RulebreakDetector {
public:
    RulebreakDetector(dpp::cluster& bot);

    // Call this from your on_message_create handler.
    // Returns true if a violation was found and handled (warned/muted/kicked/banned).
    bool check_message(const dpp::message_create_t& event);

    // Loads/saves warning counts so they survive restarts
    void load_warnings();
    void save_warnings();

    // Loads/saves active temp bans and starts the background unban watcher
    void load_bans();
    void save_bans();
    void start_unban_watcher();

private:
    dpp::cluster& m_bot;
    std::map<dpp::snowflake, WarningRecord> m_warnings; // key = user_id
    std::vector<BanRecord> m_active_bans;

    RuleViolation scan_content(const std::string& content);
    void handle_violation(const dpp::message_create_t& event, RuleViolation violation);
    std::string reason_for(RuleViolation violation);

    // simple per-user message timestamp tracking for spam/flood detection
    std::map<dpp::snowflake, std::vector<std::chrono::steady_clock::time_point>> m_recent_messages;
    bool is_flooding(dpp::snowflake user_id);
};