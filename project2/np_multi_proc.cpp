#include <sys/mman.h>
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <vector>
#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>


using namespace std;

#define err_sys(x){perror(x); exit(EXIT_FAILURE);}
#define ClientNum 30
#define MessageSize 4096
#define ClientInfoSize ClientNum *sizeof(Client)
#define fifo_path "user_pipe/"

struct Pipe{
	int fd[2];	/*read : 0, write : 1*/
	int line;
};

struct Fifo_info{
	int writefd;
	int readfd;
	int state; /*-1 : need to close, 0 : do not create, 1 : created and wait for receive*/
	char path[50];
};

struct Fifo{
	Fifo_info user_pipe[ClientNum][ClientNum];
};

struct Cmd{
	vector<string> argv;
	int pout[2] = {-1, -1}; /*index of pipes*/
	int next = -1; /*comman pipe : 0, number pipe : 1, error pipe : 2, the end of line : 3*/
	bool first = false;	/*first command of a line*/
	string file;
	int line, pipe_line = 0;
	string whole_line; /*for user pipe*/
	int sender_id=0, receiver_id=0;
	bool send_err = false, receive_err = false;
};

struct Client{
    int fd;
    int pid;
    bool used;
	char name[50];
    struct sockaddr_in client_info;
};

// shared memory file descipter
int shared_fd;
int broadcast_fd;
int fifo_fd;

// type in_addr to string
string ip_to_string(Client c){
	char ip[INET6_ADDRSTRLEN];
	sprintf(ip, "%s:%d", inet_ntoa(c.client_info.sin_addr), ntohs(c.client_info.sin_port));
	string res(ip);
	return res;
}

// transform vector string to fit execvp argument
void fit_exec_vector(vector<string> &args, char *arr[]){
	int i=0;
	for(int i=0; i<args.size(); ++i){
		arr[i] = (char *)args[i].data();
	}
	arr[args.size()]=NULL;
}

void sigchid_handler(int sig){
	int status;
	while(waitpid(-1, &status, WNOHANG) > 0){/* doing nothing */}
}

void sigint_handler(int sig){
    exit(0);
}

void Receive_broadcast(int sig){
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
    string res(message);
    cout<<res<<endl;
    munmap(message, MessageSize);
}

void Read_Fifo(int sig){
	int id = getpid(), index;
	Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
	for(int i=0; i<ClientNum; ++i){
		if(c[i].pid == id) index = i;
	}
	munmap(c, ClientInfoSize);
	Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
	for(int i=0; i<ClientNum; ++i){
		if(f->user_pipe[i][index].state == 1 && f->user_pipe[i][index].readfd == -1){
			close(f->user_pipe[i][index].readfd);
			f->user_pipe[i][index].readfd = open(f->user_pipe[i][index].path, O_NONBLOCK);
		}
	}
	munmap(f, sizeof(Fifo));
}

void built_in_setenv(string line){
	istringstream in(line);
	vector<string> v;
	string temp;
	char *env_name;
	// split command line
	while(in >> temp) v.push_back(temp);

	// check env parameter exist or not
	env_name = getenv(v[1].data());
	if(env_name != NULL){
		// if exist overwrite it
		setenv(v[1].data(), v[2].data(), 1);
	}
	else{
		// add an new environment variable
		setenv(v[1].data(), v[2].data(), 0);
	}
}

void built_in_printenv(string line){
	istringstream in(line);
	vector<string> v;
	string temp;
	// split command line
	while(in >> temp) v.push_back(temp);

	// check env parameter exist or not
	char *env_name = getenv(v[1].data());
	if(env_name) cout<< env_name <<endl;
}

void HandleLogin(int clientId){
    // welcome message
    cout<<"****************************************"<<endl;
    cout<<"** Welcome to the information server. **"<<endl;
    cout<<"****************************************"<<endl;
    // broadcast login message
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
    string ip = ip_to_string(c[clientId]), name(c[clientId].name);
    string broadcast = "*** User '"+ name +"' entered from " + ip + ". ***";
    // broadcast message stores in shared memory
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
    strcpy(message, broadcast.c_str());
    munmap(message, MessageSize);
    // signal every exist client
    for(int i=0; i<ClientNum; ++i){
        if(c[i].used)
            kill(c[i].pid, SIGUSR1);
    }

    munmap(c, ClientInfoSize);
}

void HandleLogout(int socketfd, int clientId){
    // write down logout message
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
    string broadcast, name(c[clientId].name);
    c[clientId].used = false;
    broadcast = "*** User '"+ name + "' left. ***";
    // broadcast message stores in shared memory
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
    strcpy(message, broadcast.c_str());
    munmap(message, MessageSize);
    // signal every exist client(excpet myself)
    for(int i=0; i<ClientNum; ++i){
        if(clientId != i && c[i].used){
            kill(c[i].pid, SIGUSR1);
        }
    }
    munmap(c, ClientInfoSize);

    // individual process setting
    close(socketfd);
    close(0);
    close(1);
    close(2);
}

void who(int clientId){
    cout<<"<ID>\t<nickname>\t<IP:port>\t<indicate me>"<<endl;
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
    for(int i=0; i<ClientNum; ++i){
        if(c[i].used){
            cout<<(i+1)<<"\t"<<c[i].name<<"\t"<<ip_to_string(c[i]);
            if(clientId == i) cout<<"\t<-me";
            cout<<endl;
        }
    }
    munmap(c, ClientInfoSize);
}

void tell(string cmd, int clientId){
	string temp, s, broadcast;
	int id, fd=-1;
	istringstream ss(cmd);
	ss >> temp;
	ss >> id;
    // write down broadcast message
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
    string name(c[clientId].name);
	broadcast = "*** " + name + " told you ***: ";
	while(ss >> s){
		broadcast += s + " ";
	}
    // broadcast message stores in shared memory
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
    strcpy(message, broadcast.c_str());
    munmap(message, MessageSize);
    // signal client i(id)
    if(c[id-1].used){
        kill(c[id-1].pid, SIGUSR1);
    }
    else{
        cout<<"*** Error: user #"<<id<<" does not exist yet. ***"<<endl;
    }
    munmap(c, ClientInfoSize);
}

void yell(string cmd, int clientId){
	string temp, broadcast, s;
	istringstream ss(cmd);
	ss >> temp;
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
    string name(c[clientId].name);
	broadcast += "*** "+ name +" yelled ***: ";
	while (ss >> s){
		broadcast += s + " ";
	}
    // broadcast message stores in shared memory
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
    strcpy(message, broadcast.c_str());
    munmap(message, MessageSize);
    // signal every exist client
	for(int i=0; i<ClientNum; ++i){
		if(c[i].used) kill(c[i].pid, SIGUSR1);
	}
    munmap(c, ClientInfoSize);
}

void Name(string cmd, int clientId){
	string temp, broadcast, name;
	istringstream ss(cmd);
	ss >> temp;
	ss >> name;
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
	for(int i=0; i<ClientNum; ++i){
		if(c[i].name == name){
            cout<<"*** User '"<<name<<"' already exists. ***"<<endl;
			return;
		}
	}
    strcpy(c[clientId].name, name.c_str());
	broadcast += "*** User from "+ ip_to_string(c[clientId]) + " is named '"+ name + "'. ***";
    // broadcast message stores in shared memory
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
    strcpy(message, broadcast.c_str());
    munmap(message, MessageSize);
    // signal every exist client
	for(int i=0; i<ClientNum; ++i){
		if(c[i].used) kill(c[i].pid, SIGUSR1);
	}
    munmap(c, ClientInfoSize);
}


void pipe_creation(Cmd &cmd, vector<Pipe> &pipes){
	Pipe p;
	bool temp = true;
	switch (cmd.next)
	{
	case 0:
		if(pipe(p.fd) < 0) err_sys("pipe creation error");
		p.line = -1;
		cmd.pout[0] = p.fd[0];
		cmd.pout[1] = p.fd[1];
		pipes.push_back(p);
		break;
	case 1: case 2:
		for(int i=0; i<pipes.size(); i++){
			if(pipes[i].line == cmd.pipe_line){
				cmd.pout[0] = pipes[i].fd[0];
				cmd.pout[1] = pipes[i].fd[1];
				temp = false;
				break;
			}
		}
		if(temp){
			if(pipe(p.fd) < 0) err_sys("pipe creation error");
			p.line = cmd.pipe_line;
			cmd.pout[0] = p.fd[0];
			cmd.pout[1] = p.fd[1];
			pipes.push_back(p);
		}
		break;
	default:
		break;
	}
}

void broacast(int s, int r, Cmd cmd, bool type){
	char buf[4096];
	memset(buf, 0, sizeof(char)*4096);
	Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
	if(type){
		string s_name(c[s].name), r_name(c[r-1].name);
		sprintf(buf, "*** %s (#%d) just piped '%s' to %s (#%d) ***"
			, s_name.c_str(), s+1, cmd.whole_line.c_str(), r_name.c_str(), r);
	}
	else{
		string s_name(c[s-1].name), r_name(c[r].name);
		sprintf(buf, "*** %s (#%d) just received from %s (#%d) by '%s' ***"
			, r_name.c_str(), r+1, s_name.c_str(), s, cmd.whole_line.c_str());
	}
    // broadcast message stores in shared memory
    char *message = (char *)mmap(NULL, MessageSize, PROT_READ|PROT_WRITE, MAP_SHARED, broadcast_fd, 0);
	string broadcast(buf);
    strcpy(message, broadcast.c_str());
    munmap(message, MessageSize);
    // signal every exist client
	for(int i=0; i<ClientNum; ++i){
		if(c[i].used) {
			kill(c[i].pid, SIGUSR1);
		}
	}
    munmap(c, ClientInfoSize);
}

void validate_exist_user_pipe(Cmd &cmd, int s_i, int r_i, bool dir){
	string res;
	Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
	if(dir){	//send
		if(f->user_pipe[s_i][r_i].state == 1){
			cmd.send_err = true;
			cout<<"*** Error: the pipe #"<<to_string(s_i+1)<<"->#"<<to_string(r_i+1)<<" already exists. ***"<<endl;
		}
		else{
			// update state
			f->user_pipe[s_i][r_i].state = 1;
			// create
			mkfifo(f->user_pipe[s_i][r_i].path, S_IFIFO | 0666);
			// signal receiver
			Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
			if(!cmd.send_err)
				kill(c[r_i].pid, SIGUSR2);
			munmap(c, ClientInfoSize);
			// open and point
			int temp = open(f->user_pipe[s_i][r_i].path, O_WRONLY);
			f->user_pipe[s_i][r_i].writefd = temp;
		}
	}
	else{	//receive
		//cerr<<"receive state : "<<f->user_pipe[s_i][r_i].state<<endl;
		if(f->user_pipe[s_i][r_i].state == 1){
			f->user_pipe[s_i][r_i].state = -1;
			return;
		}
		cout<<"*** Error: the pipe #"<<to_string(s_i+1)<<"->#"<<to_string(r_i+1)<<" does not exist yet. ***"<<endl;
		cmd.receive_err = true;
	}
	munmap(f, sizeof(Fifo));
}

void HandleUserPipe(Cmd &cmd, int clientid, int check_id, bool dir){
	bool finds = false;
	string res;
	Client user;
	Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
	// check connection client exist or not
	if(!c[check_id-1].used) {
		cout<<"*** Error: user #"<<check_id<<" does not exist yet. ***"<<endl;
		if(dir) cmd.send_err = true;
		else cmd.receive_err = true;
		return;
	}

	if(dir){	//send
		validate_exist_user_pipe(cmd, clientid, check_id-1, dir);

	}
	else{		//receive
		validate_exist_user_pipe(cmd, check_id-1, clientid, dir);
	}
	munmap(c, ClientInfoSize);
}

void close_fifo(int clientId){
	Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
	for(int i=0; i<ClientNum; ++i){
		if(f->user_pipe[clientId][i].state != 0){
			close(f->user_pipe[clientId][i].writefd);
			f->user_pipe[clientId][i].writefd = -1;
			unlink(f->user_pipe[clientId][i].path);
		}
		if(f->user_pipe[i][clientId].state == -1){
			close(f->user_pipe[i][clientId].readfd);
			f->user_pipe[i][clientId].state = 0;
			f->user_pipe[i][clientId].readfd = -1;
			unlink(f->user_pipe[i][clientId].path);
		}
	}
	munmap(f, sizeof(Fifo));
}

void fork_and_exec_with_pipe(int clientId, vector<Cmd> &Command, vector<Pipe> &pipes){
	// Establish handler
	signal(SIGCHLD, sigchid_handler);
	pid_t pid;
	int status;
	for(int i=0; i<Command.size(); ++i){
		if(Command[i].sender_id > 0) HandleUserPipe(Command[i], clientId, Command[i].sender_id, false);
		if(Command[i].receiver_id > 0) HandleUserPipe(Command[i], clientId, Command[i].receiver_id, true);
		if(Command[i].sender_id > 0 && !Command[i].receive_err){
			broacast(Command[i].sender_id, clientId, Command[i], 0);
		}
		if(Command[i].receiver_id > 0 && !Command[i].send_err){
			usleep(2500);
			broacast(clientId, Command[i].receiver_id, Command[i], 1);
		}

		pipe_creation(Command[i], pipes);

		while((pid=fork()) < 0){
			while(waitpid(-1,&status,WNOHANG) > 0);
		}
		if(pid == (pid_t) 0){
			if(Command[i].sender_id > 0){
				if(Command[i].receive_err){
					int devNull = open("/dev/null", O_RDWR);
					dup2(devNull, STDIN_FILENO);
					close(devNull);
				}
				else{
					Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
					dup2(f->user_pipe[Command[i].sender_id-1][clientId].readfd, STDIN_FILENO);
					close(f->user_pipe[Command[i].sender_id-1][clientId].readfd);
					munmap(f, sizeof(Fifo));
				}
			}
			else if(Command[i].first){
				for(int j=0; j<pipes.size(); ++j){
					if(pipes[j].line == Command[i].line){
						dup2(pipes[j].fd[0], STDIN_FILENO);
						close(pipes[j].fd[0]);
						close(pipes[j].fd[1]);
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
				int temp = dup2(Command[i].pout[1], STDOUT_FILENO);
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
					int devNull = open("/dev/null", O_RDWR);
					dup2(devNull, STDOUT_FILENO);
					close(devNull);
				}
				else{
					// ouput to fifo
					Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
					dup2(f->user_pipe[clientId][Command[i].receiver_id-1].writefd, STDOUT_FILENO);
					close(f->user_pipe[clientId][Command[i].receiver_id-1].writefd);
					munmap(f, sizeof(Fifo));
				}
			}

			// file redirect
			if(!Command[i].file.empty()){
				// need to know which flag should choose
				// O_TRUNC is the key : overwrite the file
				int fd;
				fd = open(Command[i].file.data(), O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR);
				dup2(fd, STDOUT_FILENO);	// send stdout to file
				close(fd);
			}

			// close all pipes
			for(int i=0; i<pipes.size(); ++i){
				close(pipes[i].fd[0]);
				close(pipes[i].fd[1]);
			}

			// run exec function
			int n = Command[i].argv.size();
			char *Argv[n + 1];
			fit_exec_vector(Command[i].argv, Argv);
			if(execvp(Command[i].argv[0].data(), Argv) == -1){
                cout<<"Unknown command: ["<<Command[i].argv[0]<<"]. "<<endl;
            }
			close_fifo(clientId);
			exit(0);
		}
		else if(pid > 0){
			if(Command[i].first){
				for(int j=0; j<pipes.size(); ++j){
					if(pipes[j].line == Command[i].line){
						close(pipes[j].fd[0]);
						close(pipes[j].fd[1]);
					}
				}
			}
			else{
				if(i-1 >= 0 && Command[i-1].next==0){
					close(Command[i-1].pout[0]);
					close(Command[i-1].pout[1]);
				}
			}

			if(Command[i].next == 3 || Command[i].sender_id > 0){
				waitpid(pid, &status, 0);
			}
			close_fifo(clientId);
		}
	}
}


void split_command_line(string line, vector<Cmd> &Command, int &true_line){
    istringstream in(line);
    string s, temp = line;
	vector<string> args;
	int forward=0, back=0;

    while(in >> s) {
		// Pipe
		if(s[0] == '|' || s[0] == '!'){
			Cmd cmd;
			cmd.argv = args;
			cmd.whole_line = temp;
			// check user pipe(need to receive)
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
		//user pipe(need to send)
		else if(s.size() > 1 && s[0] == '>'){
			int id = stoi(s.substr(1, s.size()-1));
			forward = id;
		}
		//user pipe(need to receive)
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
		cmd.whole_line = temp;
		cmd.argv = args;
		if(Command.empty()){
			cmd.first = true;
			cmd.line = true_line;
		}
		if(!Command.empty() && Command.back().next == 0){
			//cmd.pin = pipes.back();
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

void UserPipeClear(int clientId){
	Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
	for(int i=0; i<ClientNum; ++i){
		// clinet is a sender
		if(f->user_pipe[clientId][i].readfd != -1){
			close(f->user_pipe[clientId][i].readfd);
			unlink(f->user_pipe[clientId][i].path);
			f->user_pipe[clientId][i].readfd = -1;
		}
		if(f->user_pipe[clientId][i].writefd != -1){
			close(f->user_pipe[clientId][i].writefd);
			unlink(f->user_pipe[clientId][i].path);
			f->user_pipe[clientId][i].writefd = -1;
		}
		f->user_pipe[clientId][i].state = 0;

		// client is a receiver
		if(f->user_pipe[i][clientId].readfd != -1){
			close(f->user_pipe[i][clientId].readfd);
			unlink(f->user_pipe[i][clientId].path);
			f->user_pipe[i][clientId].readfd = -1;
		}
		if(f->user_pipe[i][clientId].writefd != -1){
			close(f->user_pipe[i][clientId].writefd);
			unlink(f->user_pipe[i][clientId].path);
			f->user_pipe[i][clientId].writefd = -1;
		}
		f->user_pipe[i][clientId].state = 0;
	}
	munmap(f, sizeof(Fifo));
}

void np_shell(int clientId, int socket_fd){
    // Login
    HandleLogin(clientId);
    // init environment setting
    clearenv();
    setenv("PATH", "bin:.", 1);

    int true_line = 0;
    vector<Cmd> cmd;
    vector<Pipe> pipes;
    while(1){
        cout<<"% ";
        string line;
        getline(cin, line);
        if(line.substr(0, 4) == "exit"){
			UserPipeClear(clientId);
            HandleLogout(socket_fd, clientId);
			break;
		}

		if(line.empty()){
			continue;
		}
        else if(line.size() >= 3 && line.substr(0, 3) == "who"){
            who(clientId);
            ++true_line;
        }
        else if(line.size() >= 4 && line.substr(0, 4) == "tell"){
            tell(line, clientId);
            ++true_line;
        }
        else if(line.size() >= 4 && line.substr(0, 4) == "yell"){
            yell(line, clientId);
            ++true_line;
        }
        else if(line.size() >= 4 && line.substr(0, 4) == "name"){
            Name(line, clientId);
            ++true_line;
        }
		else if(line.size() >= 6 && line.substr(0, 6) == "setenv"){
			built_in_setenv(line);
			++true_line;
			continue;
		}
		else if(line.size() >= 8 && line.substr(0, 8) == "printenv"){
			built_in_printenv(line);
			++true_line;
			continue;
		}
		else{
            split_command_line(line, cmd, true_line);
            fork_and_exec_with_pipe(clientId, cmd, pipes);
			cmd.clear();
			for(int i=0; i<pipes.size(); i++){
				if(pipes[i].line <= true_line) {
					pipes.erase(pipes.begin() + i);
					i--;
				}
			}
			++true_line;
		}
    }
}

int get_ID(){
    Client *c = (Client *)mmap(NULL, ClientInfoSize, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
    int min_id;
    for(int i=0; i<ClientNum; ++i){
        if(!c[i].used){
            min_id = i;
            c[i].used = true;
            break;
        }
    }
    munmap(c, ClientInfoSize);
    return min_id;
}

void init_client_shared(){
	/* shared memory store client info */
	shared_fd = shm_open("stored client info", O_CREAT | O_RDWR, 0666);
	ftruncate(shared_fd, sizeof(Client) * ClientNum);
	Client *c =  (Client *)mmap(NULL, sizeof(Client) * ClientNum, PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0);
	for(int i = 0;i < ClientNum;++i){
		c[i].used = false;
	}
	munmap(c, sizeof(Client) * ClientNum);
}

void init_broadcast_shared(){
    broadcast_fd = shm_open("stored broadcast message", O_CREAT | O_RDWR, 0666);
    ftruncate(broadcast_fd, MessageSize);
}

void init_fifo_shared(){
	fifo_fd = shm_open("store fifo info", O_CREAT | O_RDWR, 0666);
	ftruncate(fifo_fd, sizeof(Fifo));
	Fifo *f =  (Fifo *)mmap(NULL, sizeof(Fifo), PROT_READ | PROT_WRITE, MAP_SHARED, fifo_fd, 0);
	for(int i=0; i<ClientNum; ++i){
		for(int j=0; j<ClientNum; ++j){
			f->user_pipe[i][j].readfd = -1;
			f->user_pipe[i][j].writefd = -1;
			f->user_pipe[i][j].state = 0;
			sprintf(f->user_pipe[i][j].path, "%s%d_%d", fifo_path, i+1, j+1);
		}
	}
	munmap(f, sizeof(Fifo));
}

int main(int argc, char* argv[]){
    // create clinet info shared memory
    init_client_shared();
    init_broadcast_shared();
	init_fifo_shared();
    // start to create and server
    struct sockaddr_in serveraddr;
    struct sockaddr_in addr;

    int socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(socketfd== -1){
        err_sys("socket creation error");
    }
    int yes=1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
        perror("setsockopt(SO_REUSEADDR) failed");
        return 0;
    }

    //reset server socket memory
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = INADDR_ANY;
    serveraddr.sin_port = htons(atoi(argv[1]));

    if(bind(socketfd,(const sockaddr*) &serveraddr, sizeof(sockaddr_in)) == -1){
        err_sys("socket binding error");
    }

    // backlog :　allow 100 client request connect(if buffer is full, then return error)
    if(listen(socketfd, 100) == -1){
        close(socketfd);
        err_sys("socket listening error");
    }
	while(1){
		struct sockaddr_in clientaddr;
        socklen_t addr_len = sizeof(clientaddr);
	    int connectfd = accept(socketfd, (sockaddr*)&clientaddr, &addr_len);

		if(connectfd == -1){
			close(socketfd);
			err_sys("socket accpetion error");
		}

        // if client created, then fork a child to deal with this client
        int pid, status;
        while((pid = fork()) < 0){
            while(waitpid(-1,&status,WNOHANG) > 0);
        }
        if(pid == 0){
            // input/output point to client
            dup2(connectfd, STDIN_FILENO);
            dup2(connectfd, STDOUT_FILENO);
            dup2(connectfd, STDERR_FILENO);
            close(socketfd);
            close(connectfd);

            // signal client will use
            signal(SIGUSR1, Receive_broadcast);
            signal(SIGCHLD, sigchid_handler);
			signal(SIGUSR2, Read_Fifo);

            // get client index and set client info
            int id = get_ID();
            Client *c = (Client *)mmap(NULL, sizeof(Client) * ClientNum, PROT_READ|PROT_WRITE, MAP_SHARED, shared_fd, 0);
            c[id].fd = connectfd;
            strcpy(c[id].name, "(no name)");
            c[id].pid = getpid();
            c[id].client_info = clientaddr;
            munmap(c, sizeof(Client) * ClientNum);

            // execute npshell
            np_shell(id, connectfd);
            exit(0);
        }
        else{
            signal(SIGCHLD, sigchid_handler);
            signal(SIGINT, sigint_handler);
            close(connectfd);
        }
	}
    return 0;
}
