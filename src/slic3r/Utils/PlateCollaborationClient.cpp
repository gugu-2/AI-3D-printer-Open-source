#include "PlateCollaborationClient.hpp"
#include <iostream>
#include <sstream>

PlateCollaborationClient::PlateCollaborationClient()
    : connected_(false) {
}

PlateCollaborationClient::~PlateCollaborationClient() {
    disconnect();
}

bool PlateCollaborationClient::connect(const std::string& host, int port) {
    if (connected_) return true;

    try {
        tcp::resolver resolver(io_context_);
        auto results = resolver.resolve(host, std::to_string(port));

        socket_ = std::make_unique<tcp::socket>(io_context_);
        boost::asio::connect(*socket_, results);

        connected_ = true;

        // Start read thread
        read_thread_ = std::thread([this]() {
            this->read_loop();
        });

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Connection error: " << e.what() << std::endl;
        connected_ = false;
        return false;
    }
}

void PlateCollaborationClient::disconnect() {
    if (!connected_) return;

    connected_ = false;
    try {
        if (socket_) {
            socket_->close();
        }
        io_context_.stop();
    } catch (const std::exception& e) {
        std::cerr << "Disconnect error: " << e.what() << std::endl;
    }

    if (read_thread_.joinable()) {
        read_thread_.join();
    }
}

void PlateCollaborationClient::send_message(const json& msg) {
    if (!connected_ || !socket_) return;

    try {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        std::string message = msg.dump();
        message += "\n";
        boost::asio::write(*socket_, boost::asio::buffer(message));
    } catch (const std::exception& e) {
        std::cerr << "Send error: " << e.what() << std::endl;
        connected_ = false;
    }
}

void PlateCollaborationClient::send_object_moved(int plate_index, int object_id,
                                                  const std::array<double, 3>& position,
                                                  const std::array<double, 3>& rotation) {
    json msg;
    msg["type"] = "object_moved";
    msg["plate_index"] = plate_index;
    msg["object_id"] = object_id;
    msg["position"] = position;
    msg["rotation"] = rotation;
    msg["timestamp"] = static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count());
    send_message(msg);
}

void PlateCollaborationClient::send_object_removed(int plate_index, int object_id) {
    json msg;
    msg["type"] = "object_removed";
    msg["plate_index"] = plate_index;
    msg["object_id"] = object_id;
    msg["timestamp"] = static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count());
    send_message(msg);
}

void PlateCollaborationClient::send_object_added(int plate_index, int object_id, const json& object_data) {
    json msg;
    msg["type"] = "object_added";
    msg["plate_index"] = plate_index;
    msg["object_id"] = object_id;
    msg["data"] = object_data;
    msg["timestamp"] = static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count());
    send_message(msg);
}

void PlateCollaborationClient::send_config_changed(const json& config_delta) {
    json msg;
    msg["type"] = "config_changed";
    msg["config"] = config_delta;
    msg["timestamp"] = static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count());
    send_message(msg);
}

void PlateCollaborationClient::read_loop() {
    try {
        char buffer[65536];
        std::string incomplete_line;

        while (connected_ && socket_) {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_->is_open()) break;

            std::size_t bytes_read = socket_->read_some(boost::asio::buffer(buffer));
            if (bytes_read == 0) break;

            incomplete_line.append(buffer, bytes_read);

            // Process complete lines
            std::stringstream ss(incomplete_line);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty()) {
                    try {
                        auto msg = json::parse(line);
                        process_message(msg);
                    } catch (const std::exception& e) {
                        std::cerr << "Parse error: " << e.what() << std::endl;
                    }
                } else {
                    // Keep the incomplete part for next iteration
                    std::streampos pos = ss.tellg();
                    if (pos != std::streampos(-1)) {
                        incomplete_line = incomplete_line.substr(pos);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Read loop error: " << e.what() << std::endl;
        connected_ = false;
    }
}

void PlateCollaborationClient::process_message(const json& msg) {
    if (!msg.contains("type")) return;

    std::string msg_type = msg["type"];
    if (msg_type == "object_moved" && on_object_moved_) {
        on_object_moved_(msg);
    } else if (msg_type == "object_removed" && on_object_removed_) {
        on_object_removed_(msg);
    } else if (msg_type == "object_added" && on_object_added_) {
        on_object_added_(msg);
    } else if (msg_type == "config_changed" && on_config_changed_) {
        on_config_changed_(msg);
    } else if (msg_type == "state" && on_state_sync_) {
        on_state_sync_(msg);
    }
}
