#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>


std::string status_text(int code) {
    if (code == 200) {
        return "200 OK";
    } else if (code == 400) {
        return "400 Bad Request";
    } else if (code == 403) {
        return "403 Forbidden";
    } else if (code == 404) {
        return "404 Not Found";
    } else {
        return "500 Internal Server Error";
    }
}

// Today date, make a real date later
std::string http_date() {
    return "Fri, 02 Oct 2026 00:00:00 GMT";
}

//Number of open client connections, we use atomic because (unlike ints) ++ and -- happen in one step
std::atomic<int> active_connections{0};

// Heuristic for timeouts used on HTTP/1,1 Server
int compute_timeout_ms() {
    int active = std::max(1, active_connections.load());
    return std::max(1000, 20000 / active);
}

//Returns true if the connection is trying to close the connection (so we close it)
bool wants_close(const std::string& request) {
    std::string lower = request;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find("\r\nconnection: close") != std::string::npos;
}

//send() might not send everything in one go, so it should continue while there is still content to send
// This implementation is better for images and the overall site etc
bool send_all(int client_socket, const char* data, size_t len) {
    while (len > 0) {
        ssize_t sent = send(client_socket, data, len, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent; //move past the bytes that went out
        len -= sent;
    }
    return true;
}

// With this we create the error page and then have the status code (make more fancy later :) )
void send_error(int client_socket, const std::string& version, int code, bool keep_alive) {
    std::string status = status_text(code);
    std::string body = "<html><head><title>" + status + "</title></head>"
                       "<body><h1>" + status + "</h1></body></html>";

    std::string response = version + " " + status + "\r\n";
    response += "Date: " + http_date() + "\r\n";
    response += "Content-Type: text/html\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    response += "\r\n";
    response += body;

    send_all(client_socket, response.c_str(), response.size());
}

//Result of trying to read one request from connection
enum ReadResult { READ_OK, READ_TIMEOUT, READ_CLOSED, READ_ERROR, READ_TOO_BIG };

// One complete request put into request, then pending is things received but not used yet
ReadResult read_request(int client_socket, std::string& pending,
                        std::string& request, int timeout_ms) {
    while (true) {
        size_t end = pending.find("\r\n\r\n");
        if (end != std::string::npos) {
            request = pending.substr(0, end + 4);
            pending.erase(0, end + 4);
            return READ_OK;
        }

        //Stops clients if their header is too large
        if (pending.size() > 8192) {
            return READ_TOO_BIG;
        }

        //Wait for socket or timeout (whichever comes first)
        struct pollfd pfd;
        pfd.fd = client_socket;
        pfd.events = POLLIN;
        int ready = poll(&pfd, 1, timeout_ms); // this is where it happens

        if (ready == 0) {
            return READ_TIMEOUT;
        }
        if (ready < 0) {
            return READ_ERROR;
        }

        char chunk[4096];
        ssize_t n = recv(client_socket, chunk, sizeof(chunk), 0);
        if (n == 0) {
            return READ_CLOSED; //client hung up
        }
        if (n < 0) {
            return READ_ERROR;
        }
        pending.append(chunk, n);
    }
}

//Serves request, returns true if the connection should stay open for another req (HTTP/1.1) and false if it shouldn't (HTTP/1.0)
bool serve_request(int client_socket, const std::string& request, const std::string& doc_root) {

    //Printing the request received from the client
    std::cout << "Request received from client:\n" << request << std::endl;

    std::istringstream parser(request);
    std::string method, path, version;
    parser >> method >> path >> version;

    std::cout << "Method: " << method << std::endl;
    std::cout << "Path: " << path << std::endl;
    std::cout << "Version: " << version << std::endl;

    if (method == "GET"){
        std::cout << "Method GET is successful." << std::endl;
    }
    else {
        std::cout << "Error 400 - Invalid Request" << std::endl;
        send_error(client_socket, "HTTP/1.0", 400, false);
        return false; //malformed: can't trust where the next request starts, so close
    }

    if (version == "HTTP/1.1"){
        std::cout << "Version HTTP/1.1 is successful." << std::endl;
    }
    else if (version == "HTTP/1.0"){
        std::cout << "Version HTTP/1.0 is successful." << std::endl;
    }
    else {
        std::cout << "Error 400 - Invalid Request" << std::endl;
        send_error(client_socket, "HTTP/1.0", 400, false);
        return false;
    }

    //HTTP/1.1 keeps the connection open unless the client asks to close; HTTP/1.0 always closes
    bool keep_alive = (version == "HTTP/1.1") && !wants_close(request);

    if (path == "/") {
        std::cout << "Path / is successful." << std::endl;
        path = "/index.html";
    }

    //Check the path client is asking for, can't access internal files for security :)
    if (path.find("..") != std::string::npos) {
        std::cerr << "Error 403 - Forbidden: " << path << std::endl;
        send_error(client_socket, version, 403, keep_alive);
        return keep_alive;
    }

    std::string file_path = doc_root + path;

    std::cout << "File path: " << file_path << std::endl;

    //Look up the file's metadata, we will use this for permissions as told in class
    struct stat st;
    if (stat(file_path.c_str(), &st) < 0) {
        std::cerr << "Error 404 - File not found: " << file_path << std::endl;
        send_error(client_socket, version, 404, keep_alive);
        return keep_alive;
    }

    //Directories aren't files we can send
    if (S_ISDIR(st.st_mode)) {
        std::cerr << "Error 403 - Is a directory: " << file_path << std::endl;
        send_error(client_socket, version, 403, keep_alive);
        return keep_alive;
    }

    //Only serve files that everyone on machine can read (the "other" read bit)
    if (!(st.st_mode & S_IROTH)) {
        std::cerr << "Error 403 - Not readable by all: " << file_path << std::endl;
        send_error(client_socket, version, 403, keep_alive);
        return keep_alive;
    }

    std::cout << "Attempting to open file: " << file_path << std::endl;
    std::ifstream file(file_path, std::ios::binary);

    if (!file.is_open()){
        std::cerr << "Error 404 - File not found: " << file_path << std::endl;
        send_error(client_socket, version, 404, keep_alive);
        return keep_alive;
    }
    else {
        std::cout << "File opened successfully: " << file_path << std::endl;

    }

    std::string response = version + " 200 OK\r\n";

    std::string content_type;
    if (file_path.find(".html") != std::string::npos) {
        content_type = "text/html";
    } else if (file_path.find(".css") != std::string::npos) {
        content_type = "text/css";
    } else if (file_path.find(".js") != std::string::npos) {
        content_type = "application/javascript";
    } else if (file_path.find(".png") != std::string::npos) {
        content_type = "image/png";
    } else if (file_path.find(".jpg") != std::string::npos || file_path.find(".jpeg") != std::string::npos) {
        content_type = "image/jpeg";
    } else if (file_path.find(".gif") != std::string::npos) {
        content_type = "image/gif";
    } else if (file_path.find(".txt") != std::string::npos) {
        content_type = "text/plain";
    } else {
        content_type = "application/octet-stream"; // Default binary type
    }


    response += "Date: " + http_date() + "\r\n";
    response += "Content-Length: " + std::to_string(st.st_size) + "\r\n"; //size from stat()
    response += "Content-Type: " + content_type + "\r\n";
    response += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    response += "\r\n";


    if (!send_all(client_socket, response.c_str(), response.size())) {
        return false;
    }

    //Send the file in 4KB chunks of raw bytes, never as a string, so zero bytes in images are safe.
    //gcount() is how many bytes the last read got (the final chunk is usually less than 4KB).
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        if (!send_all(client_socket, buffer, file.gcount())) {
            return false; //client went away mid-file
        }
    }
    return keep_alive;
}

//Serves requests from clients until the client is done or these is an issue
void handle_client(int client_socket, const std::string& doc_root) {
    std::string pending; //bytes received but not yet processed, kept across requests

    while (true) {
        int timeout_ms = compute_timeout_ms();
        std::string request;
        ReadResult result = read_request(client_socket, pending, request, timeout_ms);

        if (result == READ_TIMEOUT) {
            std::cout << "Connection idle for " << timeout_ms << "ms, closing." << std::endl;
            return;
        }
        if (result == READ_TOO_BIG) {
            send_error(client_socket, "HTTP/1.0", 400, false);
            return;
        }
        if (result != READ_OK) {
            return; //client closed or socket error
        }

        if (!serve_request(client_socket, request, doc_root)) {
            return;
        }
    }
}

//Runs on its own thread for each client; closes the socket no matter how handle_client returns
void client_thread(int client_socket, std::string doc_root) {
    active_connections++;
    handle_client(client_socket, doc_root);
    active_connections--;
    close(client_socket);
}

int main(int argc, char* argv[]) {
    //Defaults, so plain ./server still works
    std::string doc_root = "www";
    int port = 8500;

    //Read -document_root and -port; each flag's value is the next argument
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-document_root" && i + 1 < argc) {
            doc_root = argv[++i];
        } else if (arg == "-port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else {
            std::cerr << "Usage: " << argv[0] << " -document_root <dir> -port <num>" << std::endl;
            return 1;
        }
    }

    //Ignore SIGPIPE so a client disconnecting mid-send doesn't kill the whole server
    signal(SIGPIPE, SIG_IGN);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0) {
        std::cerr << "Failed to create server socket." << std::endl;
        return 1;
    }

    std ::cout << "Server socket created successfully." << std::endl;
int option = 1;

//ChatGPT suggestion to fix the problem of "Address already in use" error
//when restarting the server
if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0) {
    std::cerr << "Failed to set socket option." << std::endl;
    return 1;
}

struct sockaddr_in address;

address.sin_family = AF_INET;
address.sin_addr.s_addr = INADDR_ANY;
address.sin_port = htons(port);

//For Binding

if (bind(server_socket, (struct sockaddr*)&address, sizeof(address)) < 0) {
    std::cerr << "Failed to bind server socket." << std::endl;
    return 1;
}
std::cout << "Server socket bounded successfully." << std::endl;

//For Listening

if (listen(server_socket, 5) < 0) {
    std::cerr << "Failed to listen on server socket" << std::endl;
    return 1;

}
std::cout << "Server is listening - port " << port << ", document root " << doc_root << std::endl;

//Accept clients forever, handing each one off to its own thread
while (true) {
    int client_socket = accept(server_socket, nullptr, nullptr);
    if (client_socket < 0) {
        std::cerr << "Failed to connect to client." << std::endl;
        continue;
    }
    std::cout << "Client connected." << std::endl;

    std::thread(client_thread, client_socket, doc_root).detach();
}

}
