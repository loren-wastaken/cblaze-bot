#include <dpp/dpp.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <thread>
#include <map>
#include <vector>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "com.h"
#include "rulebreak-detec.h"

using json = nlohmann::json;

std::string load_token() {
    const char* token = std::getenv("CBLAZE_TOKEN");
    if (!token || std::string(token).empty()) {
        std::cerr << "CBLAZE_TOKEN environment variable is not set." << std::endl;
        std::exit(1);
    }
    return std::string(token);
}

// Rule number -> rule text (title + description), used by /r
static const std::map<int, std::string> server_rules = {
    {1,  "**1. Be respectful.**\nTreat everyone with respect. No harassment, discrimination, or personal attacks."},
    {2,  "**2. Keep discussions legal.**\nNo piracy, malware, doxxing, or illegal activities."},
    {3,  "**3. Keep it coding-related.**\nOff-topic chat is fine, but don't spam."},
    {4,  "**4. No NSFW content.**\nThis is a safe-for-work community."},
    {5,  "**5. Don't spam.**\nNo excessive messages, mentions, or advertisements."},
    {6,  "**6. Ask good questions.**\nShow what you've tried before asking for help."},
    {7,  "**7. Help others.**\nBe patient. Everyone starts somewhere."},
    {8,  "**8. Use English.**\nKeep conversations in English so everyone can participate."},
    {9,  "**9. Follow Discord's Terms of Service.**"},
    {10, "**10. Staff decisions are final.**"}
};

// message_id -> (emoji -> role_id)
std::map<dpp::snowflake, std::map<std::string, dpp::snowflake>> reaction_role_map;

const std::string STORAGE_FILE = "messages.json";

void save_reaction_roles() {
    json j;
    for (auto& [msg_id, emoji_map] : reaction_role_map) {
        json emoji_json;
        for (auto& [emoji, role_id] : emoji_map) {
            emoji_json[emoji] = std::to_string(role_id);
        }
        j[std::to_string(msg_id)] = emoji_json;
    }

    std::ofstream out(STORAGE_FILE);
    out << j.dump(2);
    std::cout << "[Storage] Saved " << reaction_role_map.size() << " reaction-role messages." << std::endl;
}

void load_reaction_roles() {
    std::ifstream in(STORAGE_FILE);
    if (!in.is_open()) {
        std::cout << "[Storage] No existing storage file found, starting fresh." << std::endl;
        return;
    }

    try {
        json j;
        in >> j;
        for (auto& [msg_id_str, emoji_json] : j.items()) {
            dpp::snowflake msg_id = std::stoull(msg_id_str);
            std::map<std::string, dpp::snowflake> emoji_map;
            for (auto& [emoji, role_id_str] : emoji_json.items()) {
                emoji_map[emoji] = std::stoull(role_id_str.get<std::string>());
            }
            reaction_role_map[msg_id] = emoji_map;
        }
        std::cout << "[Storage] Loaded " << reaction_role_map.size() << " reaction-role messages." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Storage] Failed to load: " << e.what() << std::endl;
    }
}

int main() {
    std::string token = load_token();
    dpp::cluster bot(token, dpp::i_default_intents | dpp::i_guild_members | dpp::i_message_content);

    load_reaction_roles();

    RulebreakDetector rulebreak(bot);
    rulebreak.load_warnings();
    rulebreak.load_bans();
    rulebreak.start_unban_watcher();

    Com com(2424);

    bot.on_message_create([&rulebreak](const dpp::message_create_t& event) {
    rulebreak.check_message(event);
});

    // Handle messages coming FROM the dashboard
    com.on_message = [&bot, &com](const std::string& msg) {
        try {
            json j = json::parse(msg);
            std::string action = j.value("action", "");

            if (action == "send_message") {
                dpp::snowflake channel_id = std::stoull(j.at("channel_id").get<std::string>());
                std::string content = j.at("content").get<std::string>();
                bot.message_create(dpp::message(channel_id, content));
            }
            else if (action == "create_role") {
                std::string name = j.at("name").get<std::string>();
                std::string color_hex = j.value("color", "99AAB5");
                bool hoist = j.value("hoist", false);
                bool mentionable = j.value("mentionable", false);
                dpp::snowflake guild_id = std::stoull(j.at("guild_id").get<std::string>());

                dpp::role r;
                r.set_name(name);
                r.set_color(std::stoul(color_hex, nullptr, 16));
                if (hoist) r.flags |= dpp::r_hoist;
                if (mentionable) r.flags |= dpp::r_mentionable;
                r.guild_id = guild_id;

                bot.role_create(r);
            }
            else if (action == "get_roles") {
                dpp::snowflake guild_id = std::stoull(j.at("guild_id").get<std::string>());

                bot.roles_get(guild_id, [&com](const dpp::confirmation_callback_t& cb) {
                    if (cb.is_error()) {
                        std::cerr << "[Com] Failed to fetch roles: " << cb.get_error().message << std::endl;
                        return;
                    }
                    auto role_map = cb.get<dpp::role_map>();
                    json roles_json = json::array();
                    for (auto& [id, role] : role_map) {
                        if (role.name == "@everyone") continue;
                        roles_json.push_back({
                            {"id", std::to_string(role.id)},
                            {"name", role.name},
                            {"color", role.colour}
                        });
                    }
                    com.broadcast({{"event", "roles_list"}, {"roles", roles_json}});
                });
            }
            else if (action == "create_reaction_role") {
                dpp::snowflake channel_id = std::stoull(j.at("channel_id").get<std::string>());
                std::string content = j.at("content").get<std::string>();
                auto roles_json = j.at("roles");

                bot.message_create(dpp::message(channel_id, content),
                    [&bot, roles_json](const dpp::confirmation_callback_t& cb) {
                        if (cb.is_error()) {
                            std::cerr << "[ReactionRole] Failed to post message: " << cb.get_error().message << std::endl;
                            return;
                        }
                        auto sent = cb.get<dpp::message>();

                        // Build the emoji -> role_id map, skipping malformed rows
                        std::map<std::string, dpp::snowflake> emoji_role_map;
                        for (auto& r : roles_json) {
                            std::string emoji = r.value("emoji", "");
                            std::string role_id_str = r.value("role_id", "");

                            if (emoji.empty() || role_id_str.empty()) {
                                std::cout << "[ReactionRole] Skipping malformed entry (emoji='"
                                          << emoji << "', role_id='" << role_id_str << "')" << std::endl;
                                continue;
                            }

                            try {
                                dpp::snowflake role_id = std::stoull(role_id_str);
                                emoji_role_map[emoji] = role_id;
                            } catch (const std::exception& e) {
                                std::cerr << "[ReactionRole] Bad role_id '" << role_id_str << "': " << e.what() << std::endl;
                            }
                        }

                        if (emoji_role_map.empty()) {
                            std::cerr << "[ReactionRole] No valid role mappings, aborting reaction setup." << std::endl;
                            return;
                        }

                        reaction_role_map[sent.id] = emoji_role_map;
                        save_reaction_roles();

                        std::cout << "[ReactionRole] Stored mapping for message " << sent.id
                                  << " with " << emoji_role_map.size() << " roles." << std::endl;

                        // Add reactions ONE AT A TIME, waiting for each confirmation
                        // before firing the next one. This is the ONLY place reactions
                        // get added -- do not add another loop/thread elsewhere.
                        auto emoji_queue = std::make_shared<std::vector<std::string>>();
                        for (auto& [emoji, role_id] : emoji_role_map) {
                            emoji_queue->push_back(emoji);
                        }

                        auto index = std::make_shared<size_t>(0);
                        auto add_next = std::make_shared<std::function<void()>>();

                        *add_next = [&bot, sent, emoji_queue, index, add_next]() {
                            if (*index >= emoji_queue->size()) {
                                std::cout << "[ReactionRole] Finished adding all reactions." << std::endl;
                                return;
                            }

                            std::string emoji = (*emoji_queue)[*index];
                            (*index)++;

                            bot.message_add_reaction(sent, emoji,
                                [add_next, emoji](const dpp::confirmation_callback_t& cb) {
                                    if (cb.is_error()) {
                                        std::cerr << "[ReactionRole] Failed to add reaction '" << emoji
                                                  << "': " << cb.get_error().message << std::endl;
                                    } else {
                                        std::cout << "[ReactionRole] Added reaction: " << emoji << std::endl;
                                    }

                                    std::thread([add_next]() {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                                        (*add_next)();
                                    }).detach();
                                });
                        };

                        (*add_next)();
                    });
            }
        } catch (const std::exception& e) {
            std::cerr << "[Com] Bad command: " << e.what() << std::endl;
        }
    };

    // Run websocket server on its own thread so it doesn't block the bot
    std::thread ws_thread([&com]() {
        com.start();
    });

    bot.on_log(dpp::utility::cout_logger());

    bot.on_slashcommand([&com](const dpp::slashcommand_t& event) {
        if (event.command.get_command_name() == "ping") {
            event.reply("Cblaze here. Pong \xe2\x80\x94 blazing fast. \xf0\x9f\x94\xa5");
            com.broadcast({{"event", "ping"}, {"user", event.command.usr.username}});
        }
        else if (event.command.get_command_name() == "r") {
            int64_t rule_num = std::get<int64_t>(event.get_parameter("rule"));

            auto it = server_rules.find(static_cast<int>(rule_num));
            if (it == server_rules.end()) {
                event.reply(dpp::message(
                    "\xe2\x9d\x8c There's no rule #" + std::to_string(rule_num) +
                    ". Valid rules are 1-" + std::to_string(server_rules.size()) + "."
                ).set_flags(dpp::m_ephemeral));
                return;
            }

            event.reply(it->second);
        }
    });

    // Single, consolidated reaction handler with debug logging.
    // Do not add a second on_message_reaction_add anywhere else.
    bot.on_message_reaction_add([&bot](const dpp::message_reaction_add_t& event) {
        std::cout << "[Reaction] msg=" << event.message_id
                  << " emoji=" << event.reacting_emoji.name
                  << " user=" << event.reacting_user.username << std::endl;

        if (event.reacting_user.id == bot.me.id) {
            std::cout << "[Reaction] Ignoring bot's own reaction." << std::endl;
            return;
        }

        auto it = reaction_role_map.find(event.message_id);
        if (it == reaction_role_map.end()) {
            std::cout << "[Reaction] No mapping found for message " << event.message_id << std::endl;
            return;
        }

        auto emoji_it = it->second.find(event.reacting_emoji.name);
        if (emoji_it == it->second.end()) {
            std::cout << "[Reaction] Emoji '" << event.reacting_emoji.name << "' not in mapping." << std::endl;
            return;
        }

        bot.guild_member_add_role(event.reacting_guild.id, event.reacting_user.id, emoji_it->second,
            [](const dpp::confirmation_callback_t& cb) {
                if (cb.is_error()) {
                    std::cerr << "[Reaction] Failed to add role: " << cb.get_error().message << std::endl;
                } else {
                    std::cout << "[Reaction] Role added successfully!" << std::endl;
                }
            });
    });

    bot.on_ready([&bot](const dpp::ready_t& event) {
        if (dpp::run_once<struct register_bot_commands>()) {
            bot.global_command_create(
                dpp::slashcommand("ping", "Check if Cblaze is alive", bot.me.id)
            );

            dpp::slashcommand rule_cmd("r", "Show a server rule", bot.me.id);
            rule_cmd.add_option(
                dpp::command_option(dpp::co_integer, "rule", "The rule number to show", true)
                    .set_min_value(1)
                    .set_max_value(static_cast<int64_t>(server_rules.size()))
            );
            bot.global_command_create(rule_cmd);
        }
    });

    bot.start(dpp::st_wait);

    com.stop();
    ws_thread.join();
    return 0;
}
