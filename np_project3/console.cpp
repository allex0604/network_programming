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
    int index = 0;
    while(ss.good()){
        string s;
        getline(ss, s, '&');
        if(s[0] == 'h'){
            if(s.size() == 3) break;
            client[index][0] = s.substr(3, s.size()-3); 
        }
        else if(s[0] == 'p'){
            client[index][1] = s.substr(3, s.size()-3); 
        }
        else{
            client[index][2] = s.substr(3, s.size()-3); 
            index++; 
        }
    }
    return index;
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
  server(int num):m_resolver(io_context){
    for(int i=0; i<num; ++i){
      tcp::resolver::query query(client[i][0], client[i][1]);
      auto handler = boost::bind(&server::resolver_handler, this, i, boost::asio::placeholders::error,boost::asio::placeholders::iterator);
      m_resolver.async_resolve(query, handler);
    }
  }
private:
  void resolver_handler(int index, const boost::system::error_code &ec, boost::asio::ip::tcp::resolver::iterator it){
    if(!ec){
      auto handler = boost::bind(&server::connection_handler, this, index, boost::asio::placeholders::error);
      socket[index] = new tcp::socket(io_context);
      (*socket[index]).async_connect(*it, handler);
    }
  }

  void connection_handler(int index, const boost::system::error_code &ec){
    if(!ec){
      std::make_shared<Console_session>(std::move(*socket[index]), index)->start();
    }
    else{
       std::cerr << "connection Error: " << ec.message() << " value = " << ec.value() << "\n";
    }
  }
  ip::tcp::resolver m_resolver;
  ip::tcp::socket *socket[5];
  string host, port;
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
    client = vector<vector<string>>(5, vector<string>(3, "")); 
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