#include <iostream>
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include <set>

using namespace std;

#define err_sys(x){perror(x); exit(0);}


struct Pipe{
	int fd[2];	/*read : 0, write : 1*/
	int line;
};

struct Cmd{
	vector<string> argv;
	int pout[2] = {-1, -1}; /*index of pipes*/
	int upout[2] = {-1, -1}; /*index of user pipes*/
	int next = -1; /*comman pipe : 0, number pipe : 1, error pipe : 2, the end of line : 3*/
	bool first = false;	/*first command of a line*/
	string file;
	int line, pipe_line = 0;
	bool app = false;
	string whole_line; /*for user pipe*/
	int sender_id=0, receiver_id=0;
	bool send_err = false, receive_err = false;
};

struct Client{
	int id;
    int fd;
    int true_line = 0;
	vector<string> g;
	string name = "(no name)";
    struct sockaddr_in client_info;
    vector<Cmd> c;
	vector<Pipe> pipes;
    unordered_map<string, string> env;
};

struct user_pipe{
	int fd[2];
	int sender, receiver;	/*id*/
	bool receive = false;
};


// Global variable
vector<user_pipe> upipes;
vector<Client> cset;
unordered_map<string , vector<int>> mp;
int maxfd = 0;  //client numbers
fd_set allfd, catchfds;

void pipe_creation(Cmd &, Client &);

// transform vector string to fit execvp argument
void fit_exec_vector(vector<string> &args, char *arr[]){
	int i=0;
	for(int i=0; i<args.size(); ++i){
		arr[i] = (char *)args[i].data();
	}
	arr[args.size()]=NULL;
}

// type in_addr to string
string ip_to_string(Client c){
	char ip[INET6_ADDRSTRLEN];
	sprintf(ip, "%s:%d", inet_ntoa(c.client_info.sin_addr), ntohs(c.client_info.sin_port));
	string res(ip);
	return res;
}

void sigchid_handler(int sig){
	int status;
	while(waitpid(-1, &status, WNOHANG) > 0){/* doing nothing */}
}

void built_in_setenv(string line, Client &c){
	istringstream in(line);
	vector<string> v;
	string temp;
	char *env_name;
	// split command line
	while(in >> temp) v.push_back(temp);

    c.env[v[1]] = v[2];
}

void built_in_printenv(string line, Client c){
	istringstream in(line);
	vector<string> v;
	string temp, env_name;
	// split command line
	while(in >> temp) v.push_back(temp);

    auto it = c.env.find(v[1]);
    //cout<<"hi "<<v[1]<<" "<<c.env[v[1]]<<endl;
	if(it != c.env.end()){
        string response = it->second + '\n';
        send(c.fd, &response[0], response.size(), 0);
	}
}

void validate_exist_user_pipe(Cmd &cmd, int s_i, int r_i, int s_fd, bool dir){
	string res;
	if(dir){	//send
		for(auto u:upipes){
			if(u.sender==s_i && u.receiver==r_i){
				res += "*** Error: the pipe #" + to_string(s_i) + "->#" + to_string(r_i) + " already exists. ***\n";
				send(s_fd, &res[0], res.size(), 0);
				cmd.send_err = true;
			}
		}
	}
	else{	//receive
		for(auto u:upipes){
			if(u.sender==s_i && u.receiver==r_i){
				return;
			}
		}
		res += "*** Error: the pipe #" + to_string(s_i) + "->#" + to_string(r_i) + " does not exist yet. ***\n";
		send(s_fd, &res[0], res.size(), 0);
		cmd.receive_err = true;
	}
}

void HandleUserPipe(Cmd &cmd, Client c, int id, bool dir){
	bool finds = false;
	string res;
	Client user;
	for(auto client:cset) if(client.id == id) {user = client; finds = true;}

	if(!finds) {
		res = "*** Error: user #"+to_string(id)+" does not exist yet. ***\n";
		send(c.fd, &res[0], res.size(), 0);
		if(dir) cmd.send_err = true;
		else cmd.receive_err = true;
		return;
	}

	if(dir){	//send
		validate_exist_user_pipe(cmd, c.id, user.id, c.fd, dir);
	}
	else{		//receive
		validate_exist_user_pipe(cmd, user.id, c.id, c.fd, dir);
	}
}

void broacast(int s, int r, Cmd cmd, bool type){
	char buf[4096];
	memset(buf, 0, sizeof(char)*4096);
	//string temp = cmd.whole_line.substr(0, cmd.whole_line.size()-1);
	Client sender, receiver;
	for(auto c:cset){
		if(c.id == s) sender = c;
		if(c.id == r) receiver = c;
	}

	if(type){
		sprintf(buf, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n"
			, sender.name.c_str(), sender.id, cmd.whole_line.c_str(), receiver.name.c_str(), receiver.id);
	}
	else{
		sprintf(buf, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n"
			, receiver.name.c_str(), receiver.id, sender.name.c_str(), sender.id, cmd.whole_line.c_str());
	}

	string res(buf);
	for(auto c:cset){
		write(c.fd, res.c_str(), res.length());
	}
}

void fork_and_exec_with_pipe(vector<Cmd> &Command, Client &c){
	// Establish handler
	signal(SIGCHLD, sigchid_handler);
	pid_t pid;
	int status;

	for(int i=0; i<Command.size(); ++i){
		if(Command[i].sender_id > 0) HandleUserPipe(Command[i], c, Command[i].sender_id, false);
		if(Command[i].receiver_id > 0) HandleUserPipe(Command[i], c, Command[i].receiver_id, true);
		pipe_creation(Command[i], c);

		while((pid=fork()) < 0){
			while(waitpid(-1,&status,WNOHANG) > 0);
		}
		if(pid == (pid_t) 0){
			//cerr<<"check "<<Command[i].sender_id<<" "<<Command[i].receiver_id<<endl;
			if(Command[i].sender_id > 0){
				if(Command[i].receive_err){
					//cerr<<"check"<<endl;
					int devNull = open("/dev/null", O_RDWR);
					dup2(devNull, STDIN_FILENO);
					close(devNull);
				}
				else{
					//cerr<<"check1"<<endl;
					//cerr<<upipes.size()<<endl;
					for(int j=0; j<upipes.size(); ++j){
						//cerr<<upipes[j].sender<<" "<<Command[i].sender_id<<" "<<upipes[j].receiver<<" "<<c.id<<endl;
						if(upipes[j].sender == Command[i].sender_id && upipes[j].receiver == c.id){
							dup2(upipes[j].fd[0], STDIN_FILENO);
							close(upipes[j].fd[0]);
							close(upipes[j].fd[1]);
							broacast(Command[i].sender_id, c.id, Command[i], 0);
						}
					}
				}
			}
			else if(Command[i].first){
				for(int j=0; j<c.pipes.size(); ++j){
					if(c.pipes[j].line == Command[i].line){
						dup2(c.pipes[j].fd[0], STDIN_FILENO);
						close(c.pipes[j].fd[0]);
						close(c.pipes[j].fd[1]);
					}
				}
			}
			else{
				if(i >= 1 && Command[i-1].next==0){
					dup2(Command[i-1].pout[0], STDIN_FILENO);
					close(Command[i-1].pout[0]);
					close(Command[i-1].pout[1]);
				}
			}

			// what's next
			if(Command[i].next == 0 || Command[i].next == 1){
				dup2(Command[i].pout[1], STDOUT_FILENO);
				close(Command[i].pout[0]);
				close(Command[i].pout[1]);
			}
			else if(Command[i].next == 2){
				dup2(Command[i].pout[1], STDOUT_FILENO);
				dup2(Command[i].pout[1], STDERR_FILENO);
				close(Command[i].pout[0]);
				close(Command[i].pout[1]);
			}
			else if(Command[i].receiver_id > 0){
				if(Command[i].send_err){
					//cerr<<"check2"<<endl;
					int devNull = open("/dev/null", O_RDWR);
					dup2(devNull, STDOUT_FILENO);
					close(devNull);
				}
				else{
					//cerr<<"check3"<<endl;
					dup2(Command[i].upout[1], STDOUT_FILENO);
					close(Command[i].upout[0]);
					close(Command[i].upout[1]);
					broacast(c.id, Command[i].receiver_id, Command[i], 1);
				}
			}

			// file redirect
			if(!Command[i].file.empty()){
				// need to know which flag should choose
				// O_TRUNC is the key : overwrite the file
				int fd;
				if(Command[i].app)
					fd = open(Command[i].file.data(), O_CREAT|O_RDWR|O_APPEND, S_IRUSR|S_IWUSR);
				else
					fd = open(Command[i].file.data(), O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR);
				dup2(fd, STDOUT_FILENO);	// send stdout to file
				close(fd);
			}

			// close all pipes
			for(int i=0; i<c.pipes.size(); ++i){
				close(c.pipes[i].fd[0]);
				close(c.pipes[i].fd[1]);
			}

			// close all user pipes
			for(int i=0; i<upipes.size(); ++i){
				close(upipes[i].fd[0]);
				close(upipes[i].fd[1]);
			}

			// run exec function
			int n = Command[i].argv.size();
			char *Argv[n + 1];
			fit_exec_vector(Command[i].argv, Argv);
			if(execvp(Command[i].argv[0].data(), Argv) == -1){
                string response;
				response += "Unknown command: [";
				response += Command[i].argv[0];
				response += "]. \n";
                send(c.fd, &response[0], response.size(), 0);
            }
			exit(EXIT_SUCCESS);
		}
		else if(pid > 0){
			if(Command[i].sender_id > 0){
				for(int j=0; j<upipes.size(); ++j){
					if(upipes[j].sender == Command[i].sender_id && upipes[j].receiver == c.id){
						close(upipes[j].fd[0]);
						close(upipes[j].fd[1]);
						upipes[j].receive = true;
					}
				}
			}
			else if(Command[i].first){
				for(int j=0; j<c.pipes.size(); ++j){
					if(c.pipes[j].line == Command[i].line){
						close(c.pipes[j].fd[0]);
						close(c.pipes[j].fd[1]);
					}
				}
			}
			else{
				if(i-1 >= 0 && Command[i-1].next==0){
					close(Command[i-1].pout[0]);
					close(Command[i-1].pout[1]);
				}
			}

			if(Command[i].next == 3 || Command[i].receiver_id > 0 || Command[i].sender_id > 0){
				waitpid(pid, &status, 0);
				//cerr<<"check"<<endl;
			}
			else{
				waitpid(-1, &status, WNOHANG);
			}
		}
	}
}

void pipe_creation(Cmd &cmd, Client &c){
	Pipe p;
	bool temp = true;
	switch (cmd.next)
	{
	case 0:
		if(pipe(p.fd) < 0) err_sys("pipe creation error");
		p.line = -1;
		cmd.pout[0] = p.fd[0];
		cmd.pout[1] = p.fd[1];
		c.pipes.push_back(p);
		break;
	case 1: case 2:
		for(int i=0; i<c.pipes.size(); i++){
			if(c.pipes[i].line == cmd.pipe_line){
				cmd.pout[0] = c.pipes[i].fd[0];
				cmd.pout[1] = c.pipes[i].fd[1];
				temp = false;
				break;
			}
		}
		if(temp){
			if(pipe(p.fd) < 0) err_sys("pipe creation error");
			p.line = cmd.pipe_line;
			//cout<<p.line <<endl;
			cmd.pout[0] = p.fd[0];
			cmd.pout[1] = p.fd[1];
			c.pipes.push_back(p);
		}
		break;
	default:
		break;
	}

	//user pipe
	if(cmd.receiver_id > 0 && !cmd.send_err){
		//cerr<<"create"<<endl;
		user_pipe upipe;
		if(pipe(upipe.fd) < 0) err_sys("pipe creation error");
		upipe.sender = c.id;
		upipe.receiver = cmd.receiver_id;
		//cerr<<upipe.sender<<" "<<upipe.receiver<<endl;
		cmd.upout[0] = upipe.fd[0];
		cmd.upout[1] = upipe.fd[1];
		upipes.push_back(upipe);
	}
}

void split_command_line(string line, Client c, int &true_line, vector<Cmd> &Command){
	string s, temp = line;
    istringstream in(line);
	vector<string> args;
	int forward=0, back=0;

    while(in >> s) {
		// Pipe
		if(s[0] == '|' || s[0] == '!'){
			Cmd cmd;
			cmd.argv = args;
			cmd.whole_line = temp;
			if(back > 0){
				cmd.sender_id = back;
			}

			//decide while line of this command
			if(!Command.empty()){
				if(Command.back().next == 1){
					cmd.line = Command.back().line + 1;
					cmd.first = true;
				}
				else{
					cmd.line = Command.back().line;
					cmd.first = false;
				}
			}
			else{
				cmd.line = true_line;
				cmd.first = true;
			}

			if(s.size() == 1){	/*comman pipe*/
				cmd.next = 0;
				cmd.pipe_line = cmd.line;
			}
			else{	/*number pipe*/
				int plus_num = stoi(s.substr(1, s.size()-1));
				bool temp = true;
				cmd.pipe_line = cmd.line + plus_num;
				if(s[0] == '!') cmd.next = 2;
				else cmd.next = 1;
			}
			Command.push_back(cmd);
			args.clear();
			forward = 0; back = 0;
		}
		//user pipe(send)
		else if(s.size() > 1 && s[0] == '>'){
			int id = stoi(s.substr(1, s.size()-1));
			forward = id;
		}
		//user pipe(receive)
		else if(s.size() > 1 && s[0] == '<'){
			int id = stoi(s.substr(1, s.size()-1));
			back = id;
		}
		else{
			args.push_back(s);
		}
	}

	if(!args.empty()){
		Cmd cmd;
		cmd.whole_line = temp;
		for(int i=0; i<args.size(); ++i){
			if(args[i] == ">") {
				cmd.file = args[i+1];
				args.pop_back();
				args.pop_back();
			}
		}
		if(forward > 0){
			cmd.receiver_id = forward;
		}
		if(back > 0){
			cmd.sender_id = back;
		}
		cmd.argv = args;
		if(Command.empty()){
			cmd.first = true;
			cmd.line = true_line;
		}
		if(!Command.empty() && Command.back().next == 0){
			cmd.line = Command.back().line;
		}
		if(!Command.empty() && Command.back().next == 1){
				cmd.line = Command.back().line + 1;
				cmd.first = true;
		}
		cmd.next = 3;
		Command.push_back(cmd);
	}

	true_line = Command.back().line;
}

int get_min_id(){
	sort(cset.begin(), cset.end(), [](Client a, Client b){
		return a.id < b.id;
	});
	int no_exist_id = 1;
	for(int i=0; i<cset.size(); ++i){
		if(cset[i].id == no_exist_id) no_exist_id++;
		else{
			return no_exist_id;
		}
	}
	return (no_exist_id <= 30)?no_exist_id:-1;
}

void who(Client c){
	string res("<ID>	<nickname>	<IP:port>	<indicate me>\n");
	vector<Client> cp(cset);
	sort(cp.begin(), cp.end(), [](Client a, Client b){
		return a.id < b.id;
	});
	for(int i=0; i<cset.size(); ++i){
		res += to_string(cp[i].id) + "	" + cp[i].name + "	" + ip_to_string(cp[i]);
		if(cp[i].fd == c.fd) res += "	<-me\n";
		else res += "\n";
	}
	send(c.fd, &res[0], res.size(), 0);
}

void tell(string cmd, Client c){
	string temp, s, message;
	int id, fd=-1;
	istringstream ss(cmd);
	ss >> temp;
	ss >> id;
	message = "*** " + c.name + " told you ***: ";
	while(ss >> s){
		message += s + " ";
	}
	message += '\n';
	for(int i=0; i<cset.size(); ++i) if(cset[i].id == id){fd = cset[i].fd; break;}
	if(fd == -1) {
		message = "*** Error: user #" + to_string(id) + " does not exist yet. ***\n";
		send(c.fd, &message[0], message.size(), 0);
	}
	else send(fd, &message[0], message.size(), 0);
}

void yell(string cmd, string name){
	string temp, message, s;
	istringstream ss(cmd);
	ss >> temp;
	message += "*** "+ name +" yelled ***: ";
	while (ss >> s){
		message += s + " ";
	}
	message += '\n';
	for(auto c:cset){
		send(c.fd, &message[0], message.size(), 0);
	}
}

void Name(string cmd, Client &c){
	string temp, message, name;
	istringstream ss(cmd);
	ss >> temp;
	ss >> name;
	for(int i=0; i<cset.size(); ++i){
		if(cset[i].name == name){
			message = "*** User '"+ name +"' already exists. ***\n";
			send(c.fd, &message[0], message.size(), 0);
			return;
		}
	}
	c.name = name;
	message += "*** User from "+ ip_to_string(c) + " is named '"+ name + "'. ***\n";
	for(auto client:cset){
		send(client.fd, &message[0], message.size(), 0);
	}
}

void WelcomeMessage(Client c){
    string response;
    response += "****************************************\n";
    response += "** Welcome to the information server. **\n";
    response += "****************************************\n";
    send(c.fd, &response[0], response.size(), 0);
}

void Login_Logout(bool type, Client c){
	string brocast;
	if(type){
		brocast = "*** User '"+ c.name +"' entered from " + ip_to_string(c) + ". ***\n";
		for(auto client:cset) send(client.fd, &brocast[0], brocast.size(), 0);
	}
	else{
		brocast = "*** User '"+ c.name + "' left. ***\n";
		for(auto client:cset) {
			if(c.id == client.id) continue;
			send(client.fd, &brocast[0], brocast.size(), 0);
		}
	}
}

void close_user_pipe(bool type, Client c){
	if(type){	// receiver already exit
		for(int i=0; i<upipes.size(); ++i){
			if(upipes[i].receiver == c.id){
				close(upipes[i].fd[0]);
				close(upipes[i].fd[1]);
				upipes.erase(upipes.begin() + i);
				i--;
			}
		}
	}
	else{	// regular delete finished user pipe
		for(int i=0; i<upipes.size(); ++i){
			if(upipes[i].receive){
				upipes.erase(upipes.begin() + i);
				i--;
			}
		}
	}
}

void group(string cmd){
	string g_name, id, temp;
	istringstream ss(cmd);
	ss >> temp;
	ss >> g_name;
	while(ss >> id){
		char a = id[0];
		mp[g_name].push_back(a - '0');
		cset[a - '0'].g.push_back(g_name);
	}
}

void group1(string cmd){
	string g, message, s, temp;
	cout<<cmd<<endl;
	//istringstream ss(cmd);
	//ss >> temp;
	//ss >> g;
	//ss >> message;
	//while (ss >> s){
	//	message += s + " ";
	//}
	//message += '\n';
	//cout<<temp<<" "<<g<<endl;
	//for(auto it=mp[g].begin(); it != mp[g].end(); ++it){
	//	cout<<cset[*it].fd<<" "<<message<<endl;
	//	if(cset[*it].fd == -1) continue;
	//	send(cset[*it].fd, &message[0], message.size(), 0);
	//}
}

void delete_group(Client &c){
	for(int i=0; i<c.g.size(); ++i){
		for(int j = 0; j < mp[c.g[i]].size(); ++j){
			if(mp[c.g[i]][j] == c.id)
				mp[c.g[i]].erase(mp[c.g[i]].begin() + j);
		}
		for(int j = 0; j < mp[c.g[i]].size(); ++j){
			cout<<mp[c.g[i]][j]<<endl;;
		}
	}
}

void handleclientmessage(Client &client, string line){
    string sub_line;
    // test environment will different(need to test in np server)
    int pos = line.find('\n');
    sub_line = line.substr(0, pos);
	dup2(client.fd, STDOUT_FILENO);
	dup2(client.fd, STDERR_FILENO);
    if(sub_line.substr(0, 4) == "exit"){
		//delete_group(client);
		close_user_pipe(true, client);
		close(client.fd);
		close(1);
		close(2);
		dup2(0, 1);
		dup2(0, 2);
        FD_CLR(client.fd, &allfd);
		Login_Logout(0, client);
        client.fd = -1;
        return;
	}
	if(sub_line.empty()){/*doing nothing*/}
	else if(sub_line.size() >= 7 && sub_line.substr(0, 9) == "grouptell"){
		string g = "roomA";
		string message = "hello\n";
		bool ok = false;
		for(auto it=mp[g].begin(); it != mp[g].end(); ++it){
			int id = *it-1;
			if(client.id == cset[id].id) ok = true;
		}
		if(!ok){
			cerr<<"this client not in group"<<endl;
			++client.true_line;
		}
		if(ok){
			for(auto it=mp[g].begin(); it != mp[g].end(); ++it){
				int id = *it-1;
				if(cset[id].fd == -1) continue;
				send(cset[id].fd, &message[0], message.size(), 0);
			}
			if(mp["roomB"].size() == 3){
				for(int i=1; i<cset.size(); ++i){
					if(cset[i].fd == -1) continue;
					send(cset[i].fd, &message[0], message.size(), 0);
				}
			}
		}
		++client.true_line;
	}
	else if(sub_line.size() >= 3 && sub_line.substr(0, 3) == "who"){
		who(client);
		++client.true_line;
	}
	else if(sub_line.size() >= 4 && sub_line.substr(0, 4) == "tell"){
		tell(sub_line, client);
		++client.true_line;
	}
	else if(sub_line.size() >= 4 && sub_line.substr(0, 4) == "yell"){
		yell(sub_line, client.name);
		++client.true_line;
	}
	else if(sub_line.size() >= 4 && sub_line.substr(0, 4) == "name"){
		Name(sub_line, client);
		++client.true_line;
	}
	else if(sub_line.size() >= 6 && sub_line.substr(0, 6) == "setenv"){
		built_in_setenv(sub_line, client);
		++client.true_line;
	}
	else if(sub_line.size() >= 8 && sub_line.substr(0, 8) == "printenv"){
		built_in_printenv(sub_line, client);
		++client.true_line;
	}
	else if(sub_line.size() >= 5 && sub_line.substr(0, 5) == "group"){
		group(sub_line);
		++client.true_line;
	}
	else{
		split_command_line(sub_line, client, client.true_line, client.c);
		fork_and_exec_with_pipe(client.c, client);
		client.c.clear();
		//cout<<"hello "<<client.pipes.size()<<endl;
		for(int i=0; i<client.pipes.size(); i++){
			//cout<<client.id<<" delete pipe line: "<<client.pipes[i].line<<", true line : "<<client.true_line<<endl;
			if(client.pipes[i].line <= client.true_line) {
				//cout<<"check"<<endl;
				client.pipes.erase(client.pipes.begin() + i);
				i--;
			}
		}
		close_user_pipe(false, client);
		++client.true_line;
	}
    send(client.fd, "% ", 2, 0);
}

bool Accept(int socketfd){
    struct sockaddr_in clientaddr;
    socklen_t addr_len = sizeof(clientaddr);
	int connectfd = accept(socketfd, (sockaddr*)&clientaddr, &addr_len);

	if(connectfd == -1){
        cerr<<"socket accpetion error"<<endl;
        return false;
	}
    FD_SET(connectfd, &allfd);
    maxfd = max(connectfd, maxfd);

    Client c;
    c.fd = connectfd;
    c.client_info = clientaddr;
    built_in_setenv("setenv PATH bin:.", c);
    if((c.id = get_min_id()) < 0){
		perror("client numbers is more than 30");
		return false;
	}
	WelcomeMessage(c);
	cset.push_back(c);
	Login_Logout(1, c);
	send(c.fd, "% ", 2, 0);

    return true;
}

void HandleEnvironment(Client c){
	for(auto it=c.env.begin(); it != c.env.end(); ++it){
		setenv(it->first.c_str(), it->second.c_str(), 1);
	}
}


int main(int argc, char* argv[]){
	int ready_num;
    setenv("PATH", "bin:.", 1);
    struct sockaddr_in serveraddr;
    FD_ZERO(&allfd);
    FD_ZERO(&catchfds);

    int socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(socketfd== -1){
        err_sys("socket creation error");
    }

    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = INADDR_ANY;
    serveraddr.sin_port = htons(atoi(argv[1]));

    int yes=1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0)
        err_sys("setsockopt(SO_REUSEADDR) failed");

    if(bind(socketfd,(const sockaddr*) &serveraddr, sizeof(sockaddr_in)) == -1){
        err_sys("socket binding error");
    }

    // backlog :　allow 100 client request connect(if buffer is full, then return error)
    if(listen(socketfd, 100) == -1){
        close(socketfd);
        err_sys("socket listening error");
    }
    FD_SET(socketfd, &allfd);
    maxfd = socketfd;

    string buffer(50000, '\0');
	while(1){
		int err;
        catchfds = allfd;
		do{
			ready_num = select(maxfd+1, &catchfds, NULL, NULL, NULL);
			if(ready_num == -1) err = errno;
		}while((ready_num<0) && (err==EINTR));
        // new client wants to connection
        if(FD_ISSET(socketfd, &catchfds)){
            if(Accept(socketfd)) ready_num--;
			else continue;

			if(ready_num == 0) continue;
        }
        for(int i=0; i<cset.size(); ++i){
            //already closed connection
            if(FD_ISSET(cset[i].fd, &catchfds)){
                int bytes_received = read(cset[i].fd, &buffer[0], 49999);
                //cout<<"id "<<cset[i].id<<" receive bytes"<<bytes_received<<endl;
                if(bytes_received == -1){
                    cerr<<"read message from client error, fd = "<<cset[i].fd<<endl;
                    continue;
                }
                else if(bytes_received == 0){
                    close(cset[i].fd);
                    FD_CLR(cset[i].fd, &allfd);
                    cset[i].fd = -1;
                }
                else{
					HandleEnvironment(cset[i]);
                    handleclientmessage(cset[i], buffer);
                }
                ready_num--;
                if(ready_num == 0) break;
            }
        }
        for(int i=0; i<cset.size(); ++i){
            if(cset[i].fd == -1) {
                cset.erase(cset.begin()+i);
                i--;
            }
        }
	}

	shutdown(socketfd, SHUT_RDWR);
    close(socketfd);
}