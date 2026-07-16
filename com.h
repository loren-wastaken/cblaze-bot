#pragma once
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <set>
#include <mutex>
#include <functional>

using json = nlohmann::json;
typedef websocketpp::server<websocketpp::config::asio> ws_server;

class Com {
public:
    Com(uint16_t port);
    ~Com();

    void start();          // runs the ws server (blocking, call in its own thread)
    void stop();

    void broadcast(const json& payload);  // send to all connected dashboards

    // callback fired when dashboard sends a message to the bot
    std::function<void(const std::string& msg)> on_message;

private:
    ws_server m_server;
    uint16_t m_port;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> m_connections;
    std::mutex m_conn_mutex;
    std::thread m_thread;

    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_msg(websocketpp::connection_hdl hdl, ws_server::message_ptr msg);
};