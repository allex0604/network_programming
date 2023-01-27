#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/bind/bind.hpp>

namespace asio = boost::asio;
namespace ip = boost::asio::ip;
using tcp = boost::asio::ip::tcp;
using namespace std;

boost::asio::io_context io_context;

const string command_file_path = "./test_case/";
vector<vector<string>> client;
int total_session;

const string response_header = "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
const string console_cgi_layout = 
"\
<!DOCTYPE html>\n\
<html lang=\"en\">\n\
<head>\n\
<meta charset=\"UTF-8\" />\n\
<title>NP Project 3 Console</title>\n\
<link\n\
rel=\"stylesheet\"\n\
href=\"https://stackpath.bootstrapcdn.com/bootstrap/4.1.3/css/bootstrap.min.css\"\n\
integrity=\"sha384-MCw98/SFnGE8fJT3GXwEOngsV7Zt27NXFoaoApmYm81iuXoPkFOJwJ8ERdknLPMO\"\n\
crossorigin=\"anonymous\"\n\
/>\n\
<link\n\
href=\"https://fonts.googleapis.com/css?family=Source+Code+Pro\"\n\
rel=\"stylesheet\"\n\
/>\n\
<link\n\
rel=\"icon\"\n\
type=\"image/png\"\n\
href=\"https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png\"\n\
/>\n\
<style>\n\
* {\n\
font-family: 'Source Code Pro', monospace;\n\
font-size: 1rem !important;\n\
}\n\
body {\n\
background-color: #212529;\n\
}\n\
pre {\n\
color: #cccccc;\n\
}\n\
b {\n\
color: #ffffff;\n\
}\n\
</style>\n\
</head>";


void html_transfer(string& data) {
	std::string buffer;
	buffer.reserve(data.size());
	for (size_t pos = 0; pos != data.size(); ++pos) {
		switch (data[pos]) {
		case '&':  buffer.append("&amp;");       break;
		case '\"': buffer.append("&quot;");      break;
		case '\'': buffer.append("&apos;");      break;
		case '<':  buffer.append("&lt;");        break;
		case '>':  buffer.append("&gt;");        break;
		case '\r': break;
		default:   buffer.append(&data[pos], 1); break;
		}
	}
	data.swap(buffer);
}

void output_shell(int index,string msg){
  html_transfer(msg);
  boost::replace_all(msg, "\n", "&NewLine;");
  printf("<script>document.getElementById('s%d').innerHTML += '%s';</script>",index,msg.c_str());
  fflush(stdout);
}

void output_command(int index,string msg){
  html_transfer(msg);
  boost::replace_all(msg, "\n", "&NewLine;");
  printf("<script>document.getElementById('s%d').innerHTML += '<b>%s</b>';</script>",index,msg.c_str());
  fflush(stdout);
}

int split_query(string msg, vector<vector<string>> &client){
    std::stringstream ss(msg);
    int index = 0, num = -1;
    while(ss.good()){
        string s;
        getline(ss, s, '&');
        if(s.substr(0, 2) == "sh"){
          if(s.size() == 3) break;
          client[5][0] = s.substr(3, s.size()-3);
        }
        else if(s.substr(0, 2) == "sp"){
          client[5][1] = s.substr(3, s.size()-3); 
        }
        else{
          if(s[0] == 'h'){
            if(num > 0) continue;
            if(s.size() == 3){
              num = index;
              continue;
            }
            client[index][0] = s.substr(3, s.size()-3); 
          }
          else if(s[0] == 'p'){
            if(num > 0) continue;
            client[index][1] = s.substr(3, s.size()-3); 
          }
          else{
            if(num > 0) continue;
            client[index][2] = s.substr(3, s.size()-3); 
            index++; 
          }
        }
    }
    return (num == -1)?5:num;
}


class Console_session
: public std::enable_shared_from_this<Console_session>
{
private:
  enum { max_length = 10000 };
  array<char, 10000> data_;
  string receive_msg, filename;
  ifstream command_file;
  int index;
  ip::tcp::socket _socket;

public:
  Console_session(tcp::socket socket_,int id)
    : _socket(std::move(socket_))
  {
    index = id;
    filename = command_file_path+ client[index][2];
    command_file.open(filename,ios::in);
    receive_msg = "";
  }

  void start()
  {
    do_read();
  }

private:
  void do_read()
  {
    auto self(shared_from_this());
    _socket.async_read_some(boost::asio::buffer(data_),
        [this,self](boost::system::error_code ec, std::size_t length){
        if (!ec){
          receive_msg += string(data_.data(), length);
          if(receive_msg.find("% ") != string::npos){
            output_shell(index,receive_msg);
            receive_msg = "";
            string command_line;
            getline(command_file, command_line);
            if(command_line.empty()){
              return;
            }
            command_line += '\n';
            output_command(index, command_line);
            do_write(command_line);
          }
          do_read();
        }
        });
  }

  void do_write(string command_to_npshell)
  {
    auto self(shared_from_this());
    boost::asio::async_write(_socket, boost::asio::buffer(command_to_npshell.c_str(), command_to_npshell.size()),
      [this,self](boost::system::error_code ec, std::size_t /*length*/){
        if (ec)
        {
          cerr << "do_write Error: " << ec.message() << " value = " << ec.value() << "\n";
        }
      });
  }
};


class server
{
public:
  server(int n):m_resolver(io_context){
    num = n;
    if(client[5][0] != ""){
      tcp::resolver::query query(client[5][0], client[5][1]);
      auto handler = boost::bind(&server::socks_resolver_handler, this, boost::asio::placeholders::error,boost::asio::placeholders::iterator);
      m_resolver.async_resolve(query, handler); 
      return;     
    }
  }
private:
  void socks_resolver_handler(const boost::system::error_code &ec, boost::asio::ip::tcp::resolver::iterator it){
    if(!ec){
      for(int i=0; i<num; ++i){
        if(client[i][0] == "") continue;
        auto handler = boost::bind(&server::socks_connection_handler, this, i, boost::asio::placeholders::error);
        socket[i] = new tcp::socket(io_context);
        (*socket[i]).async_connect(*it, handler);
      }
    }
  }

  void socks_connection_handler(int index, const boost::system::error_code &ec){
    if (!ec)
    {
      unsigned char socks_connection[200];
      memset(socks_connection,'\0',200);
      socks_connection[0] = 4;
      socks_connection[1] = 1;
      unsigned int port = stoi(client[index][1]);
      socks_connection[2] = port / 256;
      socks_connection[3] = port % 256;
      socks_connection[4] = 0;
      socks_connection[5] = 0;
      socks_connection[6] = 0;
      socks_connection[7] = 1;
      socks_connection[8] = 0;
      int n = client[index][0].length(), k = 0;
      while(n != 0){
        socks_connection[9+k] = client[index][0][k];
        n--; k++;
      }
      socks_connection[9+k] = 0;
      (*socket[index]).async_send(boost::asio::buffer(socks_connection, sizeof(unsigned char)*200),
        [this, index](boost::system::error_code err, size_t len) {
            if(!err) {
                (*socket[index]).async_read_some(boost::asio::buffer(socks_reply, 8), [this, index](boost::system::error_code err, size_t len) {
                    if(!err) {
                      if(socks_reply[1] == 90){
                        // connection successful
                        std::make_shared<Console_session>(std::move(*socket[index]), index)->start();
                      }
                      else{
                        // connection unsuccessful
                        return;
                      }
                    }else
                      cerr<<"read socks reply error, ec = "<<err.message()<<endl;
                });
            }
            else
              cerr<<"send socks connection request error, ec = "<<err.message()<<endl;
        });
    }
  }
  ip::tcp::resolver m_resolver;
  ip::tcp::socket *socket[5];
  unsigned char socks_reply[8];
  string host, port;
  int num;
};

void output_top_name(){
  string top_info = "";
  top_info = "<body>\n";
  top_info += "<table class=\"table table-dark table-bordered\">\n";
  top_info += "<thead>\n";
  top_info += "<tr>\n";
  for(int i=0; i<total_session; ++i)
    top_info += "<th scope=\"col\">" +  client[i][0] + ":" + client[i][1] + "</th>\n";
  top_info += "<tr>\n";
  top_info += "<thead>\n";
  top_info += "<tbody>\n";
  top_info += "<tr>\n";
  for(int i=0; i<total_session; ++i){
    top_info += "<td><pre id=\"s" + to_string(i);
    top_info += "\" class=\"mb-0\"></pre></td>\n";
  }
  top_info += "<tr>\n";
  top_info += "<tbody>\n";
  top_info += "<table>\n";
  top_info += "<body>\n";
  cout<<top_info;
}

int main(int argc, char* argv[])
{
  try
  {
    cout << response_header;
    cout << console_cgi_layout;
    string QUERY_STRING = getenv("QUERY_STRING");
    client = vector<vector<string>>(6, vector<string>(3, "")); 
    total_session = split_query(QUERY_STRING, client);
    output_top_name();
    server s(total_session);
    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}