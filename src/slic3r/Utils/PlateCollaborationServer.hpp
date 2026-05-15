#ifndef _PLATE_COLLABORATION_SERVER_HPP_
#define _PLATE_COLLABORATION_SERVER_HPP_

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

using json = nlohmann::json;
using boost::asio::ip::tcp;

class PlateCollaborationSession;
class PlateCollaborationServer;

// Shared pointer type for sessions
typedef std::shared_ptr<PlateCollaborationSession> PlateCollaborationSessionPtr;

// Session handles a single client connection
class PlateCollaborationSession : public std::enable_shared_from_this<PlateCollaborationSession> {
public:
    PlateCollaborationSession(boost::asio::io_context& io_context, PlateCollaborationServer* server);
    tcp::socket::lowest_layer_type& socket();
    void start();
    void send_message(const json& msg);

private:
    void handle_read();
    void handle_write();

    PlateCollaborationServer* server_;
    tcp::socket socket_;
    enum { max_length = 65536 };
    char data_[max_length];
    std::string session_id_;
};

// Main server class
class PlateCollaborationServer {
public:
    PlateCollaborationServer(int port = 5678);
    ~PlateCollaborationServer();

    void start();
    void stop();
    bool is_running() const { return running_; }

    // Broadcast message to all connected clients
    void broadcast_message(const json& msg, PlateCollaborationSession* sender = nullptr);

    // Get current plate state (JSON)
    json get_plate_state() const;

    // Handle client message
    void handle_client_message(PlateCollaborationSession* session, const json& msg);

    // Register/unregister sessions
    void register_session(PlateCollaborationSessionPtr session);
    void unregister_session(PlateCollaborationSession* session);

    // Callbacks for state changes
    using StateChangeCallback = std::function<void(const json&)>;
    void set_on_state_change(StateChangeCallback cb) { on_state_change_ = cb; }

private:
    void accept_connections();

    boost::asio::io_context io_context_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread server_thread_;
    bool running_;
    int port_;

    std::vector<PlateCollaborationSessionPtr> sessions_;
    mutable std::mutex sessions_mutex_;

    StateChangeCallback on_state_change_;
};

#endif // _PLATE_COLLABORATION_SERVER_HPP
