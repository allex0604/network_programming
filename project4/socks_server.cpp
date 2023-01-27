#include <iostream>
#include <utility>
#include <vector>
#include <fstream>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>

using namespace std;
using boost::asio::ip::tcp;
namespace ip = boost::asio::ip;
boost::asio::io_context io_context;


void split_string(string DSTIP, vector<unsigned int> &v){
  string s;
  stringstream ss(DSTIP);
  int i = 0;
  while(getline(ss, s, '.')){
    v[i++] = stoi(s);
  }
}

class session
  : public std::enable_shared_from_this<session>
{
public:
  session(tcp::socket socket)
    : socket_(std::move(socket)), dst_socket(io_context), m_resolver(io_context), acceptor_(io_context, tcp::endpoint(tcp::v4(), 0))
  {
    dstIP = vector<unsigned int>(4, 0);
    socks_file.open("socks.conf", ios::in);
  }

  void start()
  {
    do_read();
  }

private:
  void do_read()
  {
    auto self(shared_from_this());
    socket_.async_read_some(boost::asio::buffer(_data, max_length),
        [this, self](boost::system::error_code ec, std::size_t length){
        if (!ec){
          command = (_data[1] == 1) ? "CONNECT" : "BIND";
          DSTPORT = (_data[2] << 8) |_data[3];
          boost::asio::ip::tcp::endpoint remote_ep = socket_.remote_endpoint();
          boost::asio::ip::address remote_ad = remote_ep.address();
          REMOTE_IP = remote_ad.to_string();
          REMOTE_PORT = to_string(remote_ep.port());
          sprintf(dstip,"%u.%u.%u.%u",_data[4],_data[5],_data[6],_data[7]);
          DSTIP = dstip;
          split_string(DSTIP, dstIP);
          // socks4a need to store domain name
          if(_data[4] == 0){
            char d[200] = "";
            size_t index = 0, j = 0;
            for(index = 8; index < length; ++index){
              if(_data[index] == 0){
                index++;
                break;
              }
            }
            for(; index < length; ++index){
              if(_data[index] == 0){
                break;
              }
              else{
                d[j] = _data[index];
                j++;
              }
            }
            domain_name = d;
          }
          if(command == "CONNECT"){
            SOCKS4_Connection();
          }
          else{
            bind_connection();
          }
        }
        });
  }
  void bind_connection(){
    auto self(shared_from_this());
    //obtain bind server's port
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.listen();
    boost::asio::ip::tcp::endpoint bind_endpoint = acceptor_.local_endpoint();
    auto bind_port = bind_endpoint.port();
    Reply = (check_firewall())?"Accept":"Reject";
    reply_message[1] = (Reply == "Accept")?90:91;
    reply_message[2] = (unsigned char)(bind_port / 256);
    reply_message[3] = (unsigned char)(bind_port % 256);
    if(reply_message[1] == 91){
      print_message();
    }
    else{
      // Send SOCKS4 REPLY to SOCKS client to tell which port to connect
      socket_.async_send(boost::asio::buffer(reply_message, 8),
        [this, self](boost::system::error_code ec, std::size_t /*length*/){
          if(!ec){
            acceptor_.async_accept(dst_socket,
              [this, self](boost::system::error_code err){
                if(!err){
                  // accept successful and reply to socks client again
                  socket_.async_send(boost::asio::buffer(reply_message, 8),
                    [this, self](boost::system::error_code ec, std::size_t /*length*/){
                      if(!ec){
                        print_message();
                        do_read(3);
                      }
                  });
                }
                else{
                  // accept failed
                  Reply = "Reject";
                  reply_message[1] = 91;
                  print_message();
                }
            });
          }
        });
    }
  }

  void SOCKS4_Connection(){
    auto self(shared_from_this());
    if(domain_name.empty()){
      Reply = (check_firewall())?"Accept":"Reject";
      if(Reply == "Accept"){
        reply_message[1] = 90;
        tcp::endpoint dst_endpoint(boost::asio::ip::address::from_string(string(dstip)),DSTPORT);
        auto handler = boost::bind(&session::reply_to_client, self, boost::asio::placeholders::error);
        dst_socket.async_connect(dst_endpoint, handler);
      }
      else{
        reply_message[1] = 91;
        print_message();
      }
    }
    else{
      tcp::resolver::query query(domain_name, to_string(DSTPORT));
      m_resolver.async_resolve(query, [this, self](const boost::system::error_code &ec, ip::tcp::resolver::iterator it) {
        if (!ec) {
          reply_message[1] = 90;
          boost::asio::ip::tcp::endpoint dst_endpoint = (*it);
          DSTIP = dst_endpoint.address().to_string();
          split_string(DSTIP, dstIP);
          Reply = (check_firewall())?"Accept":"Reject";
          if(Reply == "Accept"){
            auto handler = boost::bind(&session::reply_to_client, self, boost::asio::placeholders::error);
            dst_socket.async_connect(*it, handler);
          }
          else{
            reply_message[1] = 91;
            print_message();
          }
        }
        //else
         //cerr<< "async resolve failed : " << ec.message() << " value = " << ec.value() << "\n";
      });
    }
  }

  void reply_to_client(const boost::system::error_code &err){
    auto self(shared_from_this());
    socket_.async_send(boost::asio::buffer(reply_message, 8),
      [this, self](boost::system::error_code ec, std::size_t /*length*/){
        if(!ec){
          print_message();
          do_read(3);
        }
        else{
          //cerr<<ec.message()<<endl;
        }
      });
  }
    void do_read(int num){
      auto self(shared_from_this());
      if(num & 1){
        socket_.async_read_some(boost::asio::buffer(client_data, max_length),
          [this, self](boost::system::error_code ec, std::size_t length){
            if(!ec){
              //string temp = client_data;
              //cout<<temp<<endl;
              do_write_to_dst(length);
            }
            else if (ec == boost::asio::error::eof) {
              dst_socket.async_send(boost::asio::buffer(client_data, length),
                [this, self, ec](boost::system::error_code err, std::size_t /*length*/){
                  if(err){
                    //cerr << err.message() << endl;
                  }
              });
              dst_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            }
            else{
              //cerr << ec.message() << endl;
            }
        });
      }
      if(num & 2){
        dst_socket.async_read_some(boost::asio::buffer(dst_data, max_length),
          [this, self](boost::system::error_code ec, std::size_t length){
            if(!ec){
              //string temp = dst_data;
              //cout<<temp<<endl;
              do_write_to_client(length);
            }
            else if (ec == boost::asio::error::eof) {
              socket_.async_send(boost::asio::buffer(dst_data, length),
                [this, self, ec](boost::system::error_code err, std::size_t /*length*/){
                  if(err){
                    //cerr << err.message() << endl;
                  }
              });
              socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            }
            else{
              //cerr << ec.message() << endl;
            }
        });
      }
  }

  void do_write_to_client(std::size_t len){
    auto self(shared_from_this());
    async_write(socket_, boost::asio::buffer(dst_data, len),
      [this, self](boost::system::error_code ec, std::size_t /*length*/){
        if(!ec){
          memset(dst_data, 0, max_length);
          do_read(2);
        }
        else{
          //cerr << ec.message() << endl;
        }
      }
    );
  }

  void do_write_to_dst(std::size_t len){
    auto self(shared_from_this());
    async_write(dst_socket, boost::asio::buffer(client_data, len),
      [this, self](boost::system::error_code ec, std::size_t /*length*/){
        if(!ec){
          memset(client_data, 0, max_length);
          do_read(1);
        }
        else{
          //cerr <<"check1 "<< ec.message() << endl;
        }
      }
    );
  }

  void print_message(){
    auto self(shared_from_this());
    cout<<"<S_IP>: "<<REMOTE_IP<<endl;
    cout<<"<S_PORT>: "<<REMOTE_PORT<<endl;
    cout<<"<D_IP>: "<<DSTIP<<endl;
    cout<<"<D_PORT>: "<<DSTPORT<<endl;
    cout<<"<Command>: "<<command<<endl;
    cout<<"<Reply>: "<<Reply<<endl;
    cout<<endl;
    if(Reply == "Reject"){
      async_write(socket_, boost::asio::buffer(reply_message, 8),
        [this, self](boost::system::error_code ec, std::size_t /*length*/){
          if(!ec){
            socket_.close();
            dst_socket.close();
            exit(0);
          }
      });
    }
  }

  bool check_firewall(){
    string rule;
    bool check = false;
    char cmd = (command == "BIND")?'b':'c';
    while(getline(socks_file, rule)){
      if(rule[7] == cmd){
        string p, temp, ip, num;
        stringstream ss(rule);
        ss >> p; ss >> temp; ss >> ip;
        stringstream ss1(ip);
        int i = 0;
        bool check1 = false;
        while(getline(ss1, num, '.')){
          if(num == "*"){
            i++;
            continue;
          }
          //cout<<stoi(num)<<" "<<int(dstIP[i])<<endl;
          if(stoi(num) != int(dstIP[i])){
            check1 = true;
            break;
          }
          i++;
        }
        check = (check1)?check:true;
      }
    }
    return check;
  }

  tcp::socket socket_, dst_socket;
  ip::tcp::resolver m_resolver;
  tcp::acceptor acceptor_;
  enum { max_length = 10000 };
  unsigned char _data[max_length];
  char client_data[max_length];
  char dst_data[max_length];
  unsigned int DSTPORT;
  string command;
  string DSTIP;
  vector<unsigned int> dstIP;
  char dstip[20];
  string domain_name;
  string Reply;
  string REMOTE_IP, REMOTE_PORT;
  fstream socks_file;
  char reply_message[8];
};


struct Server
{
    Server(boost::asio::io_context &io_context, short port):
        acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        do_accept();
    }
private:
    void do_accept(){
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket_)
            {
                if(!ec){
                    io_context.notify_fork(boost::asio::io_context::fork_prepare);
                    if (fork() == 0){
                        io_context.notify_fork(boost::asio::io_context::fork_child);
                        acceptor_.close();
                        make_shared<session>(move(socket_))->start();
                    }
                    else{
                        io_context.notify_fork(boost::asio::io_context::fork_parent);
                        socket_.close();
                    }
                }
                do_accept();        
            }
        );
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

    Server s(io_context, std::atoi(argv[1]));

    io_context.run();
  }
  catch (std::exception& e)
  {
    //std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}