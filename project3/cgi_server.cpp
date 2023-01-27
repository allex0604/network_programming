#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdlib.h>
#include <unistd.h>
#include <cstring>
#include <utility>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/algorithm/string.hpp>
#include <array>

#define max_session 5
#define max_server 12
#define max_file 5


namespace asio = boost::asio;
namespace ip = boost::asio::ip;
using tcp = boost::asio::ip::tcp;
using namespace std;

asio::io_service _io_service;


const string response_header = "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
const string panel_cgi_layout = 
                "<!DOCTYPE html>\n"
                "<html lang = \"en\">"
                "<head>"
                "<title>NP Project 3 Panel</title>"
                "<link rel = \"stylesheet\" href = \"https://stackpath.bootstrapcdn.com/bootstrap/4.1.3/css/bootstrap.min.css\""
                "integrity = \"sha384-MCw98/SFnGE8fJT3GXwEOngsV7Zt27NXFoaoApmYm81iuXoPkFOJwJ8ERdknLPMO\" crossorigin = \"anonymous\" />"
                "<link href = \"https://fonts.googleapis.com/css?family=Source+Code+Pro\" rel = \"stylesheet\"/>"
                "<link rel = \"icon\" type = \"image/png\" href = \"https://cdn4.iconfinder.com/data/icons/iconsimple-setting-time/512/dashboard-512.png\"/>"
                "<style>"
                "* {"
                "font - family: 'Source Code Pro', monospace;"
                "}"
                "</style>"
                "</head>"
                "<body class = \"bg-secondary pt-5\">"
                "<form action = \"console.cgi\" method = \"GET\">"
                "<table class = \"table mx-auto bg-light\" style = \"width: inherit\">"
                "<thead class = \"thead-dark\">"
                "<tr>"
                "<th scope = \"col\">#</th>"
                "<th scope = \"col\">Host</th>"
                "<th scope = \"col\">Port</th>"
                "<th scope = \"col\">Input File</th>"
                "</tr>	</thead>"
                "<tbody>";
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

const string panel_cgi_html_header_tail = 
                "<tr>"
				"<td colspan = \"3\"></td>"
				"<td> <button type = \"submit\" class = \"btn btn-info btn-block\">Run</button>	</td>"
				"</tr> </tbody> </table> </form> </body> </html>";

const string command_file_path = "./test_case/";

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


class session1 : public enable_shared_from_this<session1> {
public:
    session1(shared_ptr<ip::tcp::socket> socket_ptr_c, vector<string> client, int i)
        : socket_ptr(socket_ptr_c), socket(_io_service), m_resolver(_io_service)
    {
        index = "s" + to_string(i);
        host = client[0];
        port = client[1];
        receive_msg = "";
        command_file.open(command_file_path + client[2], ios::in);
    }
    void start(){
        auto self(shared_from_this());
        tcp::resolver::query query(host, port);
		m_resolver.async_resolve(query, [this, self](const boost::system::error_code &ec, ip::tcp::resolver::iterator it) {
			if (!ec) {
				socket.async_connect(*it, [this, self](const boost::system::error_code &ec) {
					if (!ec) {
						do_read();
					}
					else {
						cerr<< "async connection failed : " << ec.message() << " value = " << ec.value() << "\n";
					}
				});
			}
			else
				cerr<< "async resolve failed : " << ec.message() << " value = " << ec.value() << "\n";
		});
    }
private:
	void do_read() {
		auto self(shared_from_this());
		socket.async_read_some(boost::asio::buffer(_data), 
            [this, self](const boost::system::error_code &ec, size_t length) {
			if (!ec) {
				receive_msg += string(_data.data(), length);
				if (receive_msg.find("% ") != string::npos) {
                    output_shell(receive_msg);
                    receive_msg = "";
                    string command_line;
					getline(command_file, command_line);
					if(command_line.empty())
						return;
					command_line += "\n";
					output_command(command_line);
					do_write(command_line);
				}
				do_read();
			}
		});
	}
	
	void do_write(string msg_to_npshell) {
		auto self(shared_from_this());
		socket.async_write_some(boost::asio::buffer(msg_to_npshell), 
            [this, self](const boost::system::error_code &ec, size_t /*length*/) {
			if (ec)
            {
				cerr << "session do_write Error: " << ec.message() << " value = " << ec.value() << "\n";
            }
		});
	}

	void output_shell(string msg) {
		auto self(shared_from_this());
		html_transfer(msg);
		boost::replace_all(msg, "\n", "&NewLine;");
		stringstream ss;
		ss << "<script>document.getElementById('" << index << "').innerHTML += '" << msg << "';</script>" << endl;
		send_to_client = ss.str();
		socket_ptr->async_write_some(boost::asio::buffer(send_to_client),[this, self](boost::system::error_code ec, std::size_t length) {
			if (ec){
                cerr<<"send command result to client error "<< ec.message() << " value = " << ec.value() << "\n";
            }
		});
	}

	void output_command(string msg) {
		auto self(shared_from_this());
		html_transfer(msg);
		boost::replace_all(msg, "\n", "&NewLine;");
		stringstream ss;
		ss << "<script>document.getElementById('" << index << "').innerHTML += '<b>" << msg << "</b>';</script>" << endl;
		send_to_client1 = ss.str();
		socket_ptr->async_write_some(boost::asio::buffer(send_to_client1), [this, self](boost::system::error_code ec, std::size_t length) {
			if (ec){
                cerr<<"send command to client error "<< ec.message() << " value = " << ec.value() << "\n";
            }
		});
	}
    enum { max_length = 10000 };
    array<char, 10000> _data;
	string send_to_client, send_to_client1;
	string receive_msg;
    string host, port, index;
	ifstream command_file;
	ip::tcp::resolver m_resolver;
	ip::tcp::socket socket;
	shared_ptr<ip::tcp::socket> socket_ptr;
};


class session
  : public std::enable_shared_from_this<session>
{
public:
    session(shared_ptr<ip::tcp::socket> socket_ptr_c)
        : socket_ptr(socket_ptr_c)
    {
    }

    void start()
    {
        do_read();
    }

private:
    void do_read()
    {
        auto self(shared_from_this());
        socket_ptr->async_read_some(boost::asio::buffer(data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length)
            {
            if (!ec)
            {
                cout<<"obtain get request"<<endl;
                sscanf(data_, "%s %s %s\n\rHost: %s", REQUEST_METHOD, REQUEST_URI, SERVER_PROTOCOL, HTTP_HOST);
                // we just want cgi name
                char cgi[300];
                sscanf(REQUEST_URI, "%[^?]?%s", cgi, QUERY_STRING);
                string CGI(cgi);
                if(CGI.substr(1, 9) == "panel.cgi"){
                    string response_msg = "";
                    // create option server html string
                    string option_server_html = "";
                    string domain = ".cs.nctu.edu.tw";
                    for(int i=0; i<max_server; ++i){
                        string option_server = "nplinux" + to_string(i+1);
                        option_server_html += "<option value=\""+ option_server + domain + "\">" + option_server + "</option>";
                    }
                    // create option test file html string
                    string option_file_html = "";
                    for(int i=0; i<max_file; ++i){
                        string option_file = "t" + to_string(i+1) + ".txt";
                        option_file_html += "<option value=\"" + option_file + "\">" + option_file + "</option>";
                    }
                    response_msg = panel_cgi_layout;
                    for(int i=0; i<max_session; ++i){
                        string session = "<tr>"
                        "<th scope = \"row\" class = \"align-middle\">Session " + to_string(i+1) + "</th>"
                        "<td>"
                        "<div class = \"input-group\">"
                        "<select name = \"h" + to_string(i) + "\" class = \"custom-select\">"
                        "<option></option>" + option_server_html +
                        "</select>"
                        "<div class = \"input-group-append\">"
                        "<span class = \"input-group-text\">.cs.nctu.edu.tw</span>"
                        "</div> </div> </td>"
                        "<td>"
                        "<input name = \"p" + to_string(i) + "\" type = \"text\" class = \"form-control\" size = \"5\" / >"
                        "</td>"
                        "<td>"
                        "<select name = \"f" + to_string(i) + "\" class = \"custom-select\">"
                        "<option></option>" + option_file_html + "</select> </td> </tr>";
                        response_msg += session;
                    }
                    response_msg += panel_cgi_html_header_tail;
                    response_msg = response_header + response_msg;
                    do_write(response_msg);
                }
                else if(CGI.substr(1, 11) == "console.cgi"){
                    client = vector<vector<string>>(5, vector<string>(3, "")); 
                    work_sessions = split_query(string(QUERY_STRING), client);
                    string np_shell_header = "";
                    string top_info = "";
                    top_info = "<body>\n";
                    top_info += "<table class=\"table table-dark table-bordered\">\n";
                    top_info += "<thead>\n";
                    top_info += "<tr>\n";
                    for(int i=0; i<work_sessions; ++i)
                        top_info += "<th scope=\"col\">" +  client[i][0] + ":" + client[i][1] + "</th>\n";
                    top_info += "<tr>\n";
                    top_info += "<thead>\n";
                    top_info += "<tbody>\n";
                    top_info += "<tr>\n";
                    for(int i=0; i<work_sessions; ++i){
                        top_info += "<td><pre id=\"s" + to_string(i);
                        top_info += "\" class=\"mb-0\"></pre></td>\n";
                    }
                    top_info += "<tr>\n";
                    top_info += "<tbody>\n";
                    top_info += "<table>\n";
                    top_info += "<body>\n";
                    np_shell_header = response_header + console_cgi_layout + top_info;
                    do_write(np_shell_header);
                }
                else{
                    cerr << "do_read Error: " << ec.message() << " value = " << ec.value() << "\n";
                }
            }
            });
    }

    void do_write(string msg)
    {
        auto self(shared_from_this());
        socket_ptr->async_write_some(boost::asio::buffer(msg), 
            [this, self](boost::system::error_code ec, std::size_t /*length*/)
            {
                if (!ec)
                {
                    if(work_sessions > 0){
                        for(int i=0; i<work_sessions; ++i){
                            //cout<<"host : "<<client[i][0]<<endl;
                            //cout<<"port : "<<client[i][1]<<endl;
                            //cout<<"file : "<<client[i][2]<<endl;
                            make_shared<session1>(socket_ptr, client[i], i)->start();
                        }
                        client.clear();
                        work_sessions = 0;
                    }
                    else
                        socket_ptr->shutdown(tcp::socket::shutdown_both);
                }
            });
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

    enum { max_length = 10000 };
    char data_[max_length];
    shared_ptr<ip::tcp::socket> socket_ptr;
    char REQUEST_METHOD[10];
    char REQUEST_URI[10000];
    char QUERY_STRING[10000];
    char SERVER_PROTOCOL[10000];
    char HTTP_HOST[10000];
    int work_sessions;
    vector<vector<string>> client;
};


class server {
public:
	server(unsigned short port)
		: _acceptor(_io_service, ip::tcp::endpoint(ip::tcp::v4(), port)),
		socket_ptr(make_shared<ip::tcp::socket>(_io_service)) {
		do_accept();
	}

private:
	void do_accept() {
		_acceptor.async_accept(*socket_ptr, [this](boost::system::error_code ec) {
			if (!ec) make_shared<session>(socket_ptr)->start();
			socket_ptr = make_shared<ip::tcp::socket>(_io_service);
			do_accept();
		});
	}

    ip::tcp::acceptor _acceptor;
	shared_ptr<ip::tcp::socket> socket_ptr;
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
    server s(std::atoi(argv[1]));
    _io_service.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}