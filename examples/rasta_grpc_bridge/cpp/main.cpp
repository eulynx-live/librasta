#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <rasta/rasta.h>

#include "configfile.h"

#include <rasta.grpc.pb.h>
#include <rasta.pb.h>

using namespace std::chrono_literals;

static std::mutex s_busy;
static std::mutex s_rasta_busy;

// Client
static std::unique_ptr<grpc::ClientContext> s_currentContext;
static std::unique_ptr<grpc::ClientReaderWriter<sci::SciPacket, sci::SciPacket>> s_currentStream;

// Server
static grpc::ServerReaderWriter<sci::SciPacket, sci::SciPacket> *s_currentServerStream;
static grpc::ServerContext *s_currentServerContext;

static uint32_t s_remote_id = 0;
rasta_connection *s_connection = NULL;
static rasta *s_rc;

static int s_terminator_fd[2] = {-1, -1};
static int s_data_fd[2] = {-1, -1};

static std::mutex s_fifo_mutex;
static std::deque<std::vector<uint8_t>> s_message_fifo;

static unsigned int recv_msg_size = 500;

void processConnection(std::function<std::thread()> run_thread) {
    s_message_fifo.clear();

    // Data event
    if (pipe(s_data_fd) < 0) {
        perror("Failed to create pipe");
        abort();
    }
    fd_event data_event;
    memset(&data_event, 0, sizeof(fd_event));
    data_event.callback = [](void *, int) {
        // Invalidate the event
        uint64_t u;
        ssize_t ignored = read(s_data_fd[0], &u, sizeof(u));
        (void)ignored;

        std::optional<std::vector<uint8_t>> msg = std::nullopt;

        do {
            msg = std::nullopt;

            {
                std::lock_guard<std::mutex> guard(s_fifo_mutex);
                if (s_message_fifo.size()) {
                    msg = s_message_fifo.back();
                    s_message_fifo.pop_back();
                }
            }

            if (msg != std::nullopt) {
                rasta_send(s_rc, s_connection, msg.value().data(), msg.value().size());
            }
        } while (msg != std::nullopt);

        return 0;
    };
    data_event.carry_data = s_rc;
    data_event.fd = s_data_fd[0];
    enable_fd_event(&data_event);
    rasta_add_fd_event(s_rc, &data_event, EV_READABLE);

    // Terminator event
    if (pipe(s_terminator_fd) < 0) {
        perror("Failed to create pipe");
        abort();
    }
    fd_event terminator_event;
    memset(&terminator_event, 0, sizeof(fd_event));
    terminator_event.callback = [](void *carry, int) {
        // Invalidate the event
        uint64_t u;
        ssize_t ignored = read(s_terminator_fd[0], &u, sizeof(u));
        (void)ignored;

        rasta_connection *con = reinterpret_cast<rasta_connection *>(carry);
        rasta_disconnect(con);
        return 1;
    };
    terminator_event.carry_data = s_connection;
    terminator_event.fd = s_terminator_fd[0];
    enable_fd_event(&terminator_event);
    rasta_add_fd_event(s_rc, &terminator_event, EV_READABLE);

    // Forward gRPC messages to rasta
    auto forwarderThread = run_thread();

    char *buf = new char[recv_msg_size];
    int recvlen;
    while ((recvlen = rasta_recv(s_rc, s_connection, buf, recv_msg_size)) > 0) {
        static std::mutex s_busy_writing;
        std::lock_guard<std::mutex> guard(s_busy_writing);

        std::lock_guard<std::mutex> streamGuard(s_busy);
        if (s_currentStream != nullptr) {
            sci::SciPacket outPacket;
            outPacket.set_message(buf, recvlen);
            s_currentStream->Write(outPacket);
        } else if (s_currentServerStream != nullptr) {
            sci::SciPacket outPacket;
            outPacket.set_message(buf, recvlen);
            s_currentServerStream->Write(outPacket);
        } else {
        }
    }

    delete[] buf;

    {
        std::lock_guard<std::mutex> guard(s_busy);
        if (s_currentContext) {
            s_currentContext->TryCancel();
        } else if (s_currentServerContext) {
            s_currentServerContext->TryCancel();
        }
    }

    forwarderThread.join();

    rasta_disconnect(s_connection);
    s_connection = NULL;

    rasta_remove_fd_event(s_rc, &data_event);
    rasta_remove_fd_event(s_rc, &terminator_event);

    close(s_data_fd[0]);
    close(s_data_fd[1]);
    s_data_fd[0] = s_data_fd[1] = -1;

    close(s_terminator_fd[0]);
    close(s_terminator_fd[1]);
    s_terminator_fd[0] = s_terminator_fd[1] = -1;
}

bool processRasta(std::string config_path,
                  std::string rasta_channel1_address, std::string rasta_channel1_port,
                  std::string rasta_channel2_address, std::string rasta_channel2_port,
                  std::string rasta_local_id, std::string rasta_target_id,
                  std::function<std::thread()> run_thread, bool retry_connect = true) {

    unsigned long local_id = std::stoul(rasta_local_id);
    s_remote_id = std::stoul(rasta_target_id);

    struct logger_t logger;

    rasta_config_info config;
    load_configfile(&config, &logger, config_path.c_str());
    recv_msg_size = config.receive.max_recv_msg_size;

    unsigned nchannels = config.redundancy.connections.count < 2 ? config.redundancy.connections.count : 2;

    config.redundancy_remote.connections.count = nchannels;

    strcpy(config.redundancy_remote.connections.data[0].ip, rasta_channel1_address.c_str());
    config.redundancy_remote.connections.data[0].port = std::stoi(rasta_channel1_port);
    strcpy(config.redundancy_remote.connections.data[1].ip, rasta_channel2_address.c_str());
    config.redundancy_remote.connections.data[1].port = std::stoi(rasta_channel2_port);

    config.general.rasta_id_remote = s_remote_id;

    rasta_connection_config connection = {&config};

    // TODO: Assert that this is true for every known peer
    bool server = local_id > s_remote_id;
    if (server) {
        s_rc = nullptr;
        s_rc = rasta_lib_init_configuration(&config, &connection, 1, LOG_LEVEL_INFO, LOGGER_TYPE_CONSOLE);

        if (!rasta_bind(s_rc)) {
            rasta_cleanup(s_rc);
            return false;
        }

        rasta_listen(s_rc);
        while (true) {
            s_connection = rasta_accept(s_rc);
            if (s_connection) {
                processConnection(run_thread);
            }
        }
        rasta_cleanup(s_rc);

        return true;
    } else {
        bool success = false;
        do {
            s_rc = nullptr;
            s_rc = rasta_lib_init_configuration(&config, &connection, 1, LOG_LEVEL_INFO, LOGGER_TYPE_CONSOLE);

            if (!rasta_bind(s_rc)) {
                rasta_cleanup(s_rc);
                return false;
            }

            s_connection = rasta_connect(s_rc);
            if (s_connection) {
                success = true;
                processConnection(run_thread);
                rasta_cleanup(s_rc);
            }

            // If the transport layer cannot connect, we don't have a
            // delay between connection attempts without this
            sleep(1);
        } while (retry_connect && !success);
        return success;
    }
}

class RastaService final : public sci::Rasta::Service {
  public:
    RastaService(std::string config,
                 std::string rasta_channel1_address, std::string rasta_channel1_port,
                 std::string rasta_channel2_address, std::string rasta_channel2_port,
                 std::string rasta_local_id, std::string rasta_target_id)
        : _config(config), _rasta_channel1_address(rasta_channel1_address), _rasta_channel1_port(rasta_channel1_port), _rasta_channel2_address(rasta_channel2_address), _rasta_channel2_port(rasta_channel2_port), _rasta_local_id(rasta_local_id), _rasta_target_id(rasta_target_id) {}

    grpc::Status Stream(grpc::ServerContext *context, grpc::ServerReaderWriter<sci::SciPacket, sci::SciPacket> *stream) override {
        std::lock_guard<std::mutex> guard(s_rasta_busy);

        {
            std::lock_guard<std::mutex> guard(s_busy);
            s_currentServerContext = context;
            s_currentServerStream = stream;
        }

        auto forwardGrpc = [&]() {
            return std::thread([&]() {
                sci::SciPacket message;
                while (s_currentServerStream->Read(&message)) {
                    printf("Forwarding gRPC message...\n");

                    std::vector<uint8_t> msg(message.message().cbegin(), message.message().cend());

                    {
                        std::lock_guard<std::mutex> guard(s_fifo_mutex);
                        s_message_fifo.emplace_front(msg);
                    }

                    uint64_t notify_data = 1;
                    uint64_t ignore = write(s_data_fd[1], &notify_data, sizeof(uint64_t));
                    (void)ignore;
                }

                uint64_t terminate = 1;
                uint64_t ignore = write(s_terminator_fd[1], &terminate, sizeof(uint64_t));
                (void)ignore;
            });
        };

        bool success = processRasta(_config, _rasta_channel1_address, _rasta_channel1_port, _rasta_channel2_address, _rasta_channel2_port, _rasta_local_id, _rasta_target_id, forwardGrpc, false);

        {
            std::lock_guard<std::mutex> guard(s_busy);
            s_currentServerContext = nullptr;
            s_currentServerStream = nullptr;
        }

        return success ? grpc::Status::OK : grpc::Status::CANCELLED;
    }

  protected:
    std::string _config;
    std::string _rasta_channel1_address;
    std::string _rasta_channel1_port;
    std::string _rasta_channel2_address;
    std::string _rasta_channel2_port;
    std::string _rasta_local_id;
    std::string _rasta_target_id;
};

int main(int argc, char *argv[]) {
    if (argc < 9) {
        std::cout << "Usage: " << argv[0] << " <config_file> <listen_address> <target_host_ch0> <target_port_ch0> <target_host_ch1> <target_port_ch1> <local_rasta_id> <local_remote_id> <?grpc_target_address>" << std::endl;
        return 1;
    }

    std::string config(argv[1]);

    struct stat buffer;
    if (stat(config.c_str(), &buffer) < 0) {
        std::cerr << "Could not open \"" << config << "\"." << std::endl;
        return 1;
    }

    std::string server_address(argv[2]);
    std::string rasta_channel1_address(argv[3]);
    std::string rasta_channel1_port(argv[4]);
    std::string rasta_channel2_address(argv[5]);
    std::string rasta_channel2_port(argv[6]);
    std::string rasta_local_id(argv[7]);
    std::string rasta_target_id(argv[8]);
    std::string grpc_server_address;
    if (argc >= 10) {
        grpc_server_address = std::string(argv[9]);
    }

    if (grpc_server_address.length() == 0) {
        // Start a gRPC server and wait for incoming connection before doing anything RaSTA
        RastaService svc(config,
                         rasta_channel1_address, rasta_channel1_port,
                         rasta_channel2_address, rasta_channel2_port,
                         rasta_local_id, rasta_target_id);

        grpc::EnableDefaultHealthCheckService(true);
        grpc::reflection::InitProtoReflectionServerBuilderPlugin();
        grpc::ServerBuilder builder;
        // Listen on the given address without any authentication mechanism.
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        // Register "service" as the instance through which we'll communicate with
        // clients. In this case it corresponds to an *synchronous* service.
        builder.RegisterService(&svc);
        // Finally assemble the server.
        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        std::cout << "Server listening on " << server_address << std::endl;

        // Wait for the server to shutdown. Note that some other thread must be
        // responsible for shutting down the server for this call to ever return.
        server->Wait();
    } else {
        auto connectGrpc = [&]() {
            return std::thread([&]() {
                printf("Creating gRPC connection to %s...\n", grpc_server_address.c_str());
                auto channel = grpc::CreateChannel(grpc_server_address, grpc::InsecureChannelCredentials());
                auto stub = sci::Rasta::NewStub(channel);

                {
                    // Establish gRPC connection
                    std::lock_guard<std::mutex> guard(s_busy);
                    s_currentContext = std::make_unique<grpc::ClientContext>();
                    // unsigned int client_connection_timeout = 1; // seconds
                    // std::chrono::system_clock::time_point deadline =
                    //     std::chrono::system_clock::now() + std::chrono::seconds(client_connection_timeout);
                    // s_currentContext->set_deadline(deadline);
                    s_currentContext->AddMetadata("rasta-id", std::to_string(s_remote_id));
                    s_currentStream = stub->Stream(s_currentContext.get());
                }

                sci::SciPacket message;
                while (s_currentStream->Read(&message)) {
                    printf("Forwarding gRPC message...\n");

                    std::vector<uint8_t> msg(message.message().cbegin(), message.message().cend());

                    {
                        std::lock_guard<std::mutex> guard(s_fifo_mutex);
                        s_message_fifo.emplace_front(msg);
                    }

                    uint64_t notify_data = 1;
                    uint64_t ignore = write(s_data_fd[1], &notify_data, sizeof(uint64_t));
                    (void)ignore;
                }

                {
                    std::lock_guard<std::mutex> guard(s_busy);
                    s_currentStream = nullptr;
                    s_currentContext = nullptr;
                }

                uint64_t terminate = 1;
                uint64_t ignore = write(s_terminator_fd[1], &terminate, sizeof(uint64_t));
                (void)ignore;
            });
        };

        // Establish a RaSTA connection and connect to gRPC server afterwards
        processRasta(config,
                     rasta_channel1_address, rasta_channel1_port,
                     rasta_channel2_address, rasta_channel2_port,
                     rasta_local_id, rasta_target_id,
                     connectGrpc);
    }
    return 0;
}
