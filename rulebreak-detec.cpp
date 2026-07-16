#include "rulebreak-detec.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <filesystem>

using json = nlohmann::json;

const std::string WARNINGS_FILE = "warnings.json";
const std::string BANS_FILE = "bans.json";

// ---------------------------------------------------------------------
// WORD LISTS -- these are starting points, not exhaustive. Expand them
// to fit your server. Keep everything lowercase; matching is case-insensitive.
// ---------------------------------------------------------------------

// Rule 1: basic profanity/slur filter. This does NOT catch harassment that
// uses no bad words (e.g. "everyone hates you") -- that needs a human mod.
static std::vector<std::string> profanity_words;





// Rule 2: piracy / malware / doxxing-adjacent terms and link patterns
static const std::vector<std::string> illegal_terms = {
    "cracked.exe", "keygen", "warez", "torrent link", "free vbucks",
    "grabify.link", "iplogger", "steal nudes"
};

// Rule 4: NSFW keywords (kept minimal here -- extend as needed)
static const std::vector<std::string> nsfw_terms = {
    "onlyfans.com", "pornhub.com", "xvideos.com", "nsfw link"
};

static std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return out;
}

void load_profanities() {
    namespace fs = std::filesystem;

    if (!fs::exists("profanities") || !fs::is_directory("profanities")) {
        std::cout << "[RulebreakDetector] No 'profanities' directory found, "
                     "skipping profanity list (Rule 1 keyword filter disabled)." << std::endl;
        return;
    }

    for (const auto& entry : fs::directory_iterator("profanities")) {
        std::string name = entry.path().filename().string();

        // Skip project files
        if (!entry.is_regular_file())
            continue;

        if (name == "README.md" ||
            name == "LICENSE" ||
            name == "USERS.md" ||
            name == "main.cpp" ||
            name.find(".cpp") != std::string::npos ||
            name.find(".h") != std::string::npos ||
            name.find(".json") != std::string::npos ||
            name.find(".secret") != std::string::npos ||
            name.find(".html") != std::string::npos)
            continue;

        std::ifstream file(entry.path());

        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty())
                profanity_words.push_back(to_lower(line));
        }
    }
    std::cout << "[RulebreakDetector] Loaded "
              << profanity_words.size()
              << " profanity words.\n";
}

static bool contains_any(const std::string& content_lower, const std::vector<std::string>& list) {
    for (auto& term : list) {
        if (content_lower.find(term) != std::string::npos) {
            return true;
        }
    }
    return false;
}

RulebreakDetector::RulebreakDetector(dpp::cluster& bot)
    : m_bot(bot)
{
    load_profanities();
}

RuleViolation RulebreakDetector::scan_content(const std::string& content) {
    std::string lower = to_lower(content);

    if (contains_any(lower, illegal_terms)) return RuleViolation::RULE_2_ILLEGAL;
    if (contains_any(lower, nsfw_terms)) return RuleViolation::RULE_4_NSFW;
    if (contains_any(lower, profanity_words)) return RuleViolation::RULE_1_RESPECT;

    return RuleViolation::NONE;
}

bool RulebreakDetector::is_flooding(dpp::snowflake user_id) {
    auto now = std::chrono::steady_clock::now();
    auto& timestamps = m_recent_messages[user_id];

    // drop anything older than 10 seconds
    timestamps.erase(
        std::remove_if(timestamps.begin(), timestamps.end(),
            [&](auto& t) {
                return std::chrono::duration_cast<std::chrono::seconds>(now - t).count() > 10;
            }),
        timestamps.end()
    );

    timestamps.push_back(now);

    // more than 6 messages in a 10-second window counts as flooding
    return timestamps.size() > 6;
}

std::string RulebreakDetector::reason_for(RuleViolation violation) {
    switch (violation) {
        case RuleViolation::RULE_1_RESPECT:
            return "Violation of Rule 1: Be respectful (harassment/inappropriate language).";
        case RuleViolation::RULE_2_ILLEGAL:
            return "Violation of Rule 2: Keep discussions legal (piracy/malware/doxxing).";
        case RuleViolation::RULE_4_NSFW:
            return "Violation of Rule 4: No NSFW content.";
        case RuleViolation::RULE_5_SPAM:
            return "Violation of Rule 5: Don't spam.";
        default:
            return "Violation of server rules.";
    }
}

bool RulebreakDetector::check_message(const dpp::message_create_t& event) {
    // ignore bots entirely, including ourselves
    if (event.msg.author.is_bot()) return false;

    RuleViolation violation = scan_content(event.msg.content);

    if (violation == RuleViolation::NONE && is_flooding(event.msg.author.id)) {
        violation = RuleViolation::RULE_5_SPAM;
    }

    if (violation == RuleViolation::NONE) return false;

    handle_violation(event, violation);
    return true;
}

void RulebreakDetector::handle_violation(const dpp::message_create_t& event, RuleViolation violation) {
    dpp::snowflake user_id = event.msg.author.id;
    dpp::snowflake guild_id = event.msg.guild_id;
    dpp::snowflake channel_id = event.msg.channel_id;
    std::string reason = reason_for(violation);

    // delete the offending message
    m_bot.message_delete(event.msg.id, channel_id);

    auto& record = m_warnings[user_id];
    record.count++;
    record.reasons.push_back(reason);
    save_warnings();

    std::cout << "[RulebreakDetector] User " << event.msg.author.username
              << " (" << user_id << ") warned. Count=" << record.count
              << " Reason=" << reason << std::endl;

    if (record.count < 3) {
        // simple warning
        m_bot.message_create(dpp::message(channel_id,
            "\xe2\x9a\xa0\xef\xb8\x8f <@" + std::to_string(user_id) + "> " + reason +
            " (Warning " + std::to_string(record.count) + "/6)"));
    }
    else if (record.count == 3) {
        // 1-hour timeout ("mute")
        time_t timeout_until = time(nullptr) + 3600;
        m_bot.set_audit_reason("3 warnings reached: " + reason);
        m_bot.guild_member_timeout(guild_id, user_id, timeout_until,
            [this, channel_id, user_id](const dpp::confirmation_callback_t& cb) {
                if (cb.is_error()) {
                    std::cerr << "[RulebreakDetector] Failed to timeout user: "
                              << cb.get_error().message << std::endl;
                }
            });

        m_bot.message_create(dpp::message(channel_id,
            "\xf0\x9f\x94\x87 <@" + std::to_string(user_id) +
            "> has been muted for 1 hour after reaching 3 warnings. Reason: " + reason));
    }
    else if (record.count >= 6) {
        // kick + 2-day temp ban
        std::string ban_reason = reason + " (6 warnings reached: kicked and banned 2 days)";

        m_bot.set_audit_reason(ban_reason);
        m_bot.guild_ban_add(guild_id, user_id, 0 /* delete_message_seconds */,
            [this, guild_id, user_id, channel_id, ban_reason](const dpp::confirmation_callback_t& cb) {
                if (cb.is_error()) {
                    std::cerr << "[RulebreakDetector] Failed to ban user: "
                              << cb.get_error().message << std::endl;
                    return;
                }

                BanRecord ban;
                ban.guild_id = guild_id;
                ban.user_id = user_id;
                ban.unban_at = std::chrono::system_clock::now() + std::chrono::hours(48);
                m_active_bans.push_back(ban);
                save_bans();

                m_bot.message_create(dpp::message(channel_id,
                    "\xe2\x9b\x94 <@" + std::to_string(user_id) +
                    "> has been kicked and banned for 2 days after reaching 6 warnings. Reason: " + ban_reason));
            });

        // reset their warning count since they've now been fully escalated
        record.count = 0;
        save_warnings();
    }
}

// ---------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------

void RulebreakDetector::save_warnings() {
    json j;
    for (auto& [user_id, record] : m_warnings) {
        json rec;
        rec["count"] = record.count;
        rec["reasons"] = record.reasons;
        j[std::to_string(user_id)] = rec;
    }
    std::ofstream out(WARNINGS_FILE);
    out << j.dump(2);
}

void RulebreakDetector::load_warnings() {
    std::ifstream in(WARNINGS_FILE);
    if (!in.is_open()) {
        std::cout << "[RulebreakDetector] No existing warnings file, starting fresh." << std::endl;
        return;
    }
    try {
        json j;
        in >> j;
        for (auto& [user_id_str, rec] : j.items()) {
            WarningRecord record;
            record.count = rec.value("count", 0);
            record.reasons = rec.value("reasons", std::vector<std::string>{});
            m_warnings[std::stoull(user_id_str)] = record;
        }
        std::cout << "[RulebreakDetector] Loaded warnings for " << m_warnings.size() << " users." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RulebreakDetector] Failed to load warnings: " << e.what() << std::endl;
    }
}

void RulebreakDetector::save_bans() {
    json j = json::array();
    for (auto& ban : m_active_bans) {
        json b;
        b["guild_id"] = std::to_string(ban.guild_id);
        b["user_id"] = std::to_string(ban.user_id);
        b["unban_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            ban.unban_at.time_since_epoch()).count();
        j.push_back(b);
    }
    std::ofstream out(BANS_FILE);
    out << j.dump(2);
}

void RulebreakDetector::load_bans() {
    std::ifstream in(BANS_FILE);
    if (!in.is_open()) {
        std::cout << "[RulebreakDetector] No existing bans file, starting fresh." << std::endl;
        return;
    }
    try {
        json j;
        in >> j;
        for (auto& b : j) {
            BanRecord ban;
            ban.guild_id = std::stoull(b.at("guild_id").get<std::string>());
            ban.user_id = std::stoull(b.at("user_id").get<std::string>());
            long long epoch_seconds = b.at("unban_at").get<long long>();
            ban.unban_at = std::chrono::system_clock::time_point(std::chrono::seconds(epoch_seconds));
            m_active_bans.push_back(ban);
        }
        std::cout << "[RulebreakDetector] Loaded " << m_active_bans.size() << " active temp bans." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RulebreakDetector] Failed to load bans: " << e.what() << std::endl;
    }
}

void RulebreakDetector::start_unban_watcher() {
    std::thread([this]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::minutes(5));

            auto now = std::chrono::system_clock::now();
            for (auto it = m_active_bans.begin(); it != m_active_bans.end(); ) {
                if (now >= it->unban_at) {
                    dpp::snowflake guild_id = it->guild_id;
                    dpp::snowflake user_id = it->user_id;

                    m_bot.set_audit_reason("Temporary ban expired (2 days elapsed).");
                    m_bot.guild_ban_delete(guild_id, user_id,
                        [user_id](const dpp::confirmation_callback_t& cb) {
                            if (cb.is_error()) {
                                std::cerr << "[RulebreakDetector] Failed to auto-unban " << user_id
                                          << ": " << cb.get_error().message << std::endl;
                            } else {
                                std::cout << "[RulebreakDetector] Auto-unbanned user " << user_id << std::endl;
                            }
                        });

                    it = m_active_bans.erase(it);
                } else {
                    ++it;
                }
            }
            save_bans();
        }
    }).detach();
}
