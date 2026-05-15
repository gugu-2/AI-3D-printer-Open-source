#ifndef _PLATE_COLLABORATION_CLIENT_HPP_
#define _PLATE_COLLABORATION_CLIENT_HPP_

#include <string>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <queue>
#include <mutex>

using json = nlohmann::json;
using boost::asio::ip::tcp;

class PlateCollaborationClient {
public:
    PlateCollaborationClient();
    ~PlateCollaborationClient();

    // Connection management
    bool connect(const std::string& host, int port);
    void disconnect();
    bool is_connected() const { return connected_; }

    // Send messages to server
    void send_object_moved(int plate_index, int object_id,
                          const std::array<double, 3>& position,
                          const std::array<double, 3>& rotation);
    void send_object_removed(int plate_index, int object_id);
    void send_object_added(int plate_index, int object_id, const json& object_data);
    void send_config_changed(const json& config_delta);

    // Callbacks for receiving updates
    using MessageCallback = std::function<void(const json&)>;
    void set_on_object_moved(MessageCallback cb) { on_object_moved_ = cb; }
    void set_on_object_removed(MessageCallback cb) { on_object_removed_ = cb; }
    void set_on_object_added(MessageCallback cb) { on_object_added_ = cb; }
    void set_on_config_changed(MessageCallback cb) { on_config_changed_ = cb; }
    void set_on_state_sync(MessageCallback cb) { on_state_sync_ = cb; }

private:
    void send_message(const json& msg);
    void read_loop();
    void process_message(const json& msg);

    boost::asio::io_context io_context_;
    std::unique_ptr<tcp::socket> socket_;
    std::thread read_thread_;
    bool connected_;
    std::mutex socket_mutex_;

    MessageCallback on_object_moved_;
    MessageCallback on_object_removed_;
    MessageCallback on_object_added_;
    MessageCallback on_config_changed_;
    MessageCallback on_state_sync_;
};

#endif // _PLATE_COLLABORATION_CLIENT_HPP
