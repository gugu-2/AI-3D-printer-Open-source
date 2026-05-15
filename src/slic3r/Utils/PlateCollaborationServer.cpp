#include "PlateCollaborationServer.hpp"
#include <iostream>
#include <sstream>

PlateCollaborationSession::PlateCollaborationSession(boost::asio::io_context& io_context,
                                                     PlateCollaborationServer* server)
    : server_(server), socket_(io_context) {
}

tcp::socket::lowest_layer_type& PlateCollaborationSession::socket() {
    return socket_.lowest_layer();
}

void PlateCollaborationSession::start() {
    handle_read();
}

void PlateCollaborationSession::send_message(const json& msg) {
    try {
        std::string message = msg.dump();
        message += "\n"; // Delimiter for line-based protocol
        socket_.async_write_some(boost::asio::buffer(message),
            [this](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
                if (!ec) {
                    // Write successful
                }
            });
    } catch (const std::exception& e) {
        std::cerr << "Error sending message: " << e.what() << std::endl;
    }
}

void PlateCollaborationSession::handle_read() {
    socket_.async_read_some(boost::asio::buffer(data_, max_length),
        [this](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            if (!ec) {
                std::string received_data(data_, bytes_transferred);
                try {
                    // Simple line-based parsing
                    std::stringstream ss(received_data);
                    std::string line;
                    while (std::getline(ss, line)) {
                        if (!line.empty()) {
                            auto msg = json::parse(line);
                            server_->handle_client_message(this, msg);
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error parsing message: " << e.what() << std::endl;
                }
                handle_read(); // Continue reading
            }
        });
}

void PlateCollaborationSession::handle_write() {
    // Called by the write handler in send_message
}

// Server implementation
PlateCollaborationServer::PlateCollaborationServer(int port)
    : running_(false), port_(port) {
}

PlateCollaborationServer::~PlateCollaborationServer() {
    stop();
}

void PlateCollaborationServer::start() {
    if (running_) return;
    running_ = true;

    server_thread_ = std::thread([this]() {
        try {
            acceptor_ = std::make_unique<tcp::acceptor>(
                io_context_,
                tcp::endpoint(tcp::v4(), port_)
            );

            accept_connections();
            io_context_.run();
        } catch (const std::exception& e) {
            std::cerr << "Server error: " << e.what() << std::endl;
            running_ = false;
        }
    });
}

void PlateCollaborationServer::stop() {
    if (!running_) return;
    running_ = false;
    io_context_.stop();
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void PlateCollaborationServer::accept_connections() {
    auto new_session = std::make_shared<PlateCollaborationSession>(io_context_, this);
    acceptor_->async_accept(new_session->socket(),
        [this, new_session](const boost::system::error_code& ec) {
            if (!ec) {
                register_session(new_session);
                new_session->start();
            }
            accept_connections(); // Continue accepting
        });
}

void PlateCollaborationServer::broadcast_message(const json& msg, PlateCollaborationSession* sender) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& session : sessions_) {
        if (session.get() != sender) {
            session->send_message(msg);
        }
    }
}

json PlateCollaborationServer::get_plate_state() const {
    // This will be populated by the Plater through callback
    json state;
    state["type"] = "state";
    state["timestamp"] = static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count());
    return state;
}

void PlateCollaborationServer::handle_client_message(PlateCollaborationSession* session, const json& msg) {
    if (!msg.contains("type")) return;

    std::string msg_type = msg["type"];
    if (msg_type == "ping") {
        json response;
        response["type"] = "pong";
        session->send_message(response);
    } else if (on_state_change_) {
        on_state_change_(msg);
    }

    // Broadcast to other clients
    broadcast_message(msg, session);
}

void PlateCollaborationServer::register_session(PlateCollaborationSessionPtr session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.push_back(session);

    // Send current state to new client
    json state = get_plate_state();
    session->send_message(state);
}

void PlateCollaborationServer::unregister_session(PlateCollaborationSession* session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [session](const PlateCollaborationSessionPtr& s) { return s.get() == session; }),
        sessions_.end()
    );
}
