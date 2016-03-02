#include <iostream>
#include <thread>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <map>

#include <assert.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

using namespace std;

struct Settings
{
    string directory = "/tmp/www/htdocs";
    string ip = "0.0.0.0";
    string port = "15282";
    bool daemonize = true;
    int maxevents = 64;
};

class HTTPParser
{
public:
    void parseSome(const char* data, size_t count)
    {
        buffer.append(data, data + count);
        if (buffer.find("\r\n\r\n") != string::npos ||
                buffer.find("\n\n") != string::npos)
        {
            done = true;
        }
    }

    string getFilename()
    {
        size_t getpos = buffer.find("GET");
        if (getpos == string::npos)
        {
            cout << "* Unknown request" << endl;
            return string();
        }

        size_t fnpos = buffer.find('/', getpos + 3);
        if (fnpos == string::npos)
        {
            cout << "* No filename found" << endl;
            return string();
        }

        size_t efnpos = buffer.find(' ', fnpos);
        if (fnpos == string::npos)
        {
            cout << "* Broken request" << endl;
            return string();
        }

        return buffer.substr(fnpos, efnpos - fnpos);
    }

    bool ready() const { return done; }

private:
    string buffer;
    size_t current_size = 0;
    size_t position = 0;
    bool done = false;
};

int createListener(const string& ip, const string &port);
int setSocketNonblocking(int socketfd);
int epollWaitThread(int epollfd, int socketfd, Settings& settings);
int acceptIncoming(int socketfd);
string fileToHTTP(const string& filename);
void test();

int main(int argc, char *argv[])
{
    test();
    cout << "* Welcome to the HTTP Server 3000!" << endl;

    Settings settings;
    int rc;
    while ((rc = getopt(argc, argv, "h:p:d:k")) != -1)
    {
        switch (rc)
        {
        case 'h':
            settings.ip = optarg;
            break;

        case 'p':
            settings.port = optarg;
            break;

        case 'd':
            settings.directory = optarg;
            break;

        case 'k':
            settings.daemonize = false;
            break;
        }
    }

    cout << "* Checking htdocs directory" << endl;
    struct stat st;
    if (stat(settings.directory.c_str(), &st) == -1)
    {
        cout << "* Directory not found, creating htdocs directory" << endl;
        if (mkdir(settings.directory.c_str(), 755) == -1)
        {
            cout << "* Unable to create htdocs dir: error " << errno << endl;
            cout << "* Falling back to system() call" << endl;

            char call[4096];
            sprintf(call, "mkdir -p %s", settings.directory.c_str());
            if (system(call) != 0)
            {
                cout << "* Out of options: exit" << endl;
                return 1;
            }
        }
    }

    int socketfd = createListener(settings.ip, settings.port);
    if (socketfd == -1)
    {
        cout << "* Unable to create listener" << endl;
        return 1;
    }

    rc = setSocketNonblocking(socketfd);
    if (rc == -1)
    {
        cout << "* Unable to set nonblock socket mode";
        return 1;
    }

    cout << "* Starting to listen for incoming connections" << endl;
    rc = listen(socketfd, settings.maxevents);
    if (rc == -1)
    {
        cout << "* Unable to listen server socket" << endl;
        return 1;
    }

    int epollfd = epoll_create1(0);
    if (epollfd == -1)
    {
        cout << "* Unable to create epoll instance" << endl;
        return 1;
    }

    epoll_event event;
    epoll_event* events;

    event.data.fd = socketfd;
    event.events = EPOLLIN | EPOLLET;
    rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, socketfd, &event);
    if (rc == -1)
    {
        cout << "* Unable to add listener to epoll" << endl;
        return 1;
    }

    thread thread1(&epollWaitThread, epollfd, socketfd, ref(settings));
    thread thread2(&epollWaitThread, epollfd, socketfd, ref(settings));
    thread thread3(&epollWaitThread, epollfd, socketfd, ref(settings));

    if (settings.daemonize)
    {
        cout << "* This is the last message you will see, bye-bye!" << endl;
        daemon(0, 0);
    }

    thread1.join();
    thread2.join();
    thread3.join();

    return 0;
}

int createListener(const string &ip, const string& port)
{
    addrinfo hints;
    addrinfo *result, *rp;
    int rc, sfd;

    memset(&hints, 0, sizeof(addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(nullptr, port.c_str(), &hints, &result);
    if (rc != 0)
    {
        cout << "* Unable to getaddrinfo: " << gai_strerror(rc) << endl;
        return -1;
    }

    for (rp = result; rp != nullptr; rp = rp->ai_next)
    {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1)
        {
            continue;
        }

        rc = bind(sfd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0)
        {
            cout << "* Socket successfully bound" << endl;
            break;
        }

        close(sfd);
    }

    if (rp == nullptr)
    {
        cout << "* Unable to bound!" << endl;
        return -1;
    }

    freeaddrinfo(result);
    return sfd;
}

int setSocketNonblocking(int socketfd)
{
    int flags, rc;
    flags = fcntl(socketfd, F_GETFL, 0);
    if (flags == -1)
    {
        cout << "* Unable to check fcntl flags" << endl;
        return -1;
    }

    flags |= O_NONBLOCK;
    rc = fcntl(socketfd, F_SETFL, flags);
    if (rc == -1)
    {
        cout << "* Unable to set nonblock fcntl flag" << endl;
        return -1;
    }

    cout << "* Socket mode is nonblock now" << endl;

    int reuse_addr = 1;
    setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR,
               &reuse_addr, sizeof(reuse_addr));

    return 0;
}

int epollWaitThread(int epollfd, int socketfd, Settings &settings)
{
    int rc;
    int event_count;
    epoll_event event;
    epoll_event* events;
    events = static_cast<epoll_event*>(calloc(settings.maxevents, sizeof(event)));
    char read_buffer[1024];
    size_t read_buffer_size = 1024;
    while (true)
    {
        event_count = epoll_wait(epollfd, events, settings.maxevents, -1);
        for (int i = 0; i < event_count; ++i)
        {
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (!(events[i].events & EPOLLIN)))
            {
                cout << "* Error on descriptor " << events[i].data.fd << endl;
                close(events[i].data.fd);
                continue;
            }

            if (socketfd == events[i].data.fd)
            {
                int newfd = acceptIncoming(socketfd);
                event.data.fd = newfd;
                event.events = EPOLLIN | EPOLLET;
                rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, newfd, &event);
                if (rc == -1)
                {
                    cout << "* Error on epoll_ctl_add: " << errno << endl;
                    close(newfd);
                }

                continue;
            }

            if (events[i].events & EPOLLIN)
            {
                cout << "* New EPOLLIN event on socket "
                     << events[i].data.fd << endl;

                HTTPParser parser;
                do
                {
                    rc = recv(events[i].data.fd, read_buffer, read_buffer_size, 0);
                    if (rc == -1)
                    {
                        cout << "* Read error " << errno << ": "
                             << strerror(errno) << endl;
                    }
                    parser.parseSome(read_buffer, rc);
                }
                while (!parser.ready());

                string filename = parser.getFilename();
                if (filename.empty())
                {
                    cout << "* Http header error: no filename" << endl;
                }

                string output = fileToHTTP(settings.directory + filename);
                int sendpos = 0;

                while (sendpos < output.size())
                {
                    rc = send(events[i].data.fd, output.c_str() + sendpos,
                              output.size() - sendpos, 0);
                    if (rc == -1)
                    {
                        cout << "* Write error " << errno << ": "
                             << strerror(errno) << endl;
                    }

                    sendpos += rc;
                }

                cout << "* Closing connection " << events[i].data.fd << endl;
                close(events[i].data.fd);
            }
        }
    }

    free(events);
}

int acceptIncoming(int socketfd)
{
    sockaddr in_addr;
    socklen_t in_len;
    int incomfd, rc;
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

    in_len = sizeof(in_addr);
    incomfd = accept(socketfd, &in_addr, &in_len);
    if (incomfd == -1)
    {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        {
            return incomfd;
        }
        else
        {
            cout << "* Error during accept on descriptor " << socketfd << endl;
            return -1;
        }
    }

    rc = getnameinfo(&in_addr, in_len,
                     hbuf, sizeof(hbuf),
                     sbuf, sizeof(sbuf),
                     NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc == 0)
    {
        cout << "* Accepted new connection on descriptor " << incomfd << ", "
                "host " << hbuf << ":" << sbuf << endl;
    }

#if 0
    rc = setSocketNonblocking(incomfd);
    if (rc == -1)
    {
        close(incomfd);
        return -1;
    }
#endif

    return incomfd;
}

void test()
{
    cout << "* Test case 1" << endl;
    const char* request = "GET /index.html HTTP/1.1\r\n"
            "Host: www.example.com\r\n"
            "\r\n";
    const size_t request_length = strlen(request);

    HTTPParser parser;
    parser.parseSome(request, request_length);
    assert( parser.ready() );

    string fn = parser.getFilename();
    assert( fn == "/index.html" );

    cout << "* All tests OK" << endl;
}

string fileToHTTP(const string &filename)
{
    stringstream out;
    ifstream file(filename);
    if (!file.is_open())
    {
        // 404
        out << "HTTP/1.1 404 Not Found\r\n"
               "Content-Type: text/html\r\n"
               "Content-Length: 49\r\n"
               "\r\n"
               "<html><body><h1>404 Not Found</h1></body></html>\r\n";

        return out.str();
    }


    string file_str;
    file_str.assign(istreambuf_iterator<char>(file),
                    istreambuf_iterator<char>());

    out << "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html; charset=UTF-8\r\n"
           "Content-Length: " << file_str.size() << "\r\n"
        << "Server: HTTPServer3K/0.1.0 (Unix)\r\n"
           "\r\n"
        << file_str;

    return out.str();
}
