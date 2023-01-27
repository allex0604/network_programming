#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdlib.h>
#include <unistd.h>
#include <cstring>
#include <utility>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;
using namespace std;
const string status_code = "HTTP/1.1 200 OK\n";

boost::asio::io_context io_context;

void env_setting(string name, string content){
  setenv(name.c_str(), content.c_str(), 1);
}

class session
  : public std::enable_shared_from_this<session>
{
public:
  session(tcp::socket socket)
    : socket_(std::move(socket))
  {
  }

  void start()
  {
    do_read();
  }

private:
  char REQUEST_METHOD[10];
  char REQUEST_URI[10000];
  char QUERY_STRING[10000];
  char SERVER_PROTOCOL[10000];
  char HTTP_HOST[10000];
  string SERVER_ADDR;
  string SERVER_PORT;
  string REMOTE_ADDR;
  string REMOTE_PORT;
  void do_read()
  {
    auto self(shared_from_this());
    socket_.async_read_some(boost::asio::buffer(data_, max_length),
        [this, self](boost::system::error_code ec, std::size_t length)
        {
          if (!ec)
          {
            do_write(length);
          }
        });
  }

  void do_write(std::size_t length)
  {
    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(status_code.c_str(), status_code.size()),
        [this, self](boost::system::error_code ec, std::size_t /*length*/)
        {
          if (!ec)
          {
            /*obtain environment variables*/
            // obtain information from http request message
            sscanf(data_, "%s %s %s\n\rHost: %s", REQUEST_METHOD, REQUEST_URI, SERVER_PROTOCOL, HTTP_HOST);
            // obtain QUERY_STRING from REQUEST_URI
            char cgi[150];
            sscanf(REQUEST_URI, "%[^?]?%s", cgi, QUERY_STRING);
            // obtain server address and port
            boost::asio::ip::tcp::endpoint local_ep = socket_.local_endpoint();
            boost::asio::ip::address local_ad = local_ep.address();
            SERVER_ADDR = local_ad.to_string();
            SERVER_PORT = to_string(local_ep.port());
            // obtain clinet address and port
            boost::asio::ip::tcp::endpoint remote_ep = socket_.remote_endpoint();
            boost::asio::ip::address remote_ad = remote_ep.address();
            REMOTE_ADDR = remote_ad.to_string();
            REMOTE_PORT = to_string(remote_ep.port());
            /*fork processing*/
            io_context.notify_fork(boost::asio::io_context::fork_prepare);
            if (fork() == 0)
            {
              io_context.notify_fork(boost::asio::io_context::fork_child);
              /*setting env*/
              env_setting("REQUEST_METHOD", string(REQUEST_METHOD));
              env_setting("REQUEST_URI", string(REQUEST_URI));
              env_setting("SERVER_PROTOCOL", string(SERVER_PROTOCOL));
              env_setting("QUERY_STRING", string(QUERY_STRING));
              env_setting("HTTP_HOST", string(HTTP_HOST));
              env_setting("SERVER_ADDR",SERVER_ADDR);
              env_setting("SERVER_PORT",SERVER_PORT);
              env_setting("REMOTE_ADDR",REMOTE_ADDR);
              env_setting("REMOTE_PORT",REMOTE_PORT);
              /*handle dup and exec*/
              close(0);
              close(1);
							dup2(socket_.native_handle(), STDIN_FILENO);
							dup2(socket_.native_handle(), STDOUT_FILENO);
							string cgi_file = string(cgi);
							cgi_file = "."+cgi_file;
							socket_.close();
							if (execlp(cgi_file.c_str(), cgi_file.c_str(), NULL) < 0) {
								cerr<<"exec failed"<<endl;
							}
              exit(0);
            }
            else
            {
              io_context.notify_fork(boost::asio::io_context::fork_parent);
              socket_.close();
            }
            do_read();
          }
        });
  }
  tcp::socket socket_;
  enum { max_length = 1024 };
  char data_[max_length];
};

class server
{
public:
  server(boost::asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
  {
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    do_accept();
  }

private:
  void do_accept()
  {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket)
        {
          if (!ec)
          {
            std::make_shared<session>(std::move(socket))->start();
          }

          do_accept();
        });
  }

  tcp::acceptor acceptor_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 2)
    {
      std::cerr << "Usage: async_tcp_echo_server <port>\n";
      return 1;
    }

    server s(io_context, std::atoi(argv[1]));

    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}