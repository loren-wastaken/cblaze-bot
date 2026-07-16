#include "com.h"
#include <iostream>

Com::Com(uint16_t port) : m_port(port) {
    m_server.init_asio();
    m_server.clear_access_channels(websocketpp::log::alevel::all);

    m_server.set_open_handler(std::bind(&Com::on_open, this, std::placeholders::_1));
    m_server.set_close_handler(std::bind(&Com::on_close, this, std::placeholders::_1));
    m_server.set_message_handler(std::bind(&Com::on_msg, this, std::placeholders::_1, std::placeholders::_2));
}

Com::~Com() {
    stop();
}

void Com::start() {
    m_server.listen(m_port);
    m_server.start_accept();
    std::cout << "[Com] WebSocket server listening on port " << m_port << std::endl;
    m_server.run();
}

void Com::stop() {
    m_server.stop_listening();
    for (auto& hdl : m_connections) {
        websocketpp::lib::error_code ec;
        m_server.close(hdl, websocketpp::close::status::going_away, "shutdown", ec);
    }
}

void Com::broadcast(const json& payload) {
    std::string msg = payload.dump();
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    for (auto& hdl : m_connections) {
        websocketpp::lib::error_code ec;
        m_server.send(hdl, msg, websocketpp::frame::opcode::text, ec);
    }
}

void Com::on_open(websocketpp::connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    m_connections.insert(hdl);
    std::cout << "[Com] Dashboard connected." << std::endl;
}

void Com::on_close(websocketpp::connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    m_connections.erase(hdl);
    std::cout << "[Com] Dashboard disconnected." << std::endl;
}

void Com::on_msg(websocketpp::connection_hdl hdl, ws_server::message_ptr msg) {
    if (on_message) {
        on_message(msg->get_payload());
    }
}