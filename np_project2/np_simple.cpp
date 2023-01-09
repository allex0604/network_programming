#include <iostream>
#include <unistd.h>
#include <stdio.h>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <cstring>

using namespace std;

#define err_sys(x){perror(x); exit(0);}

struct Pipe{
	int fd[2];	/*read : 0, write : 1*/
	int line;
};
struct Cmd{
	vector<string> argv;
	int pout[2] = {-1, -1}; /*index of pipes*/
	int next = -1; /*comman pipe : 0, number pipe : 1, error pipe : 2, the end of line : 3*/
	bool first = false;	/*first command of a line*/
	string file;
	int line, pipe_line = 0;
	bool app = false;
};

// Global variable
vector<Pipe> pipes;

void pipe_creation(Cmd &);

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

void built_in_printenv(string line, int socket_fd){
	istringstream in(line);
	vector<string> v;
	string temp;
	char *env_name;
	// split command line
	while(in >> temp) v.push_back(temp);

	// check env parameter exist or not
	env_name = getenv(v[1].data());
	if(env_name != NULL){
        strcat(env_name, "\n");
        send(socket_fd, env_name, strlen(env_name), 0);
	}
}

void fork_and_exec_with_pipe(vector<Cmd> &Command, int socket_fd){
	// Establish handler
	signal(SIGCHLD, sigchid_handler);
	pid_t pid;
	int status;

	for(int i=0; i<Command.size(); ++i){
		pipe_creation(Command[i]);

		while((pid=fork()) < 0){
			while(waitpid(-1,&status,WNOHANG) > 0);
		}
		if(pid == (pid_t) 0){
			if(Command[i].first){
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
			for(int i=0; i<pipes.size(); ++i){
				close(pipes[i].fd[0]);
				close(pipes[i].fd[1]);
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
                send(socket_fd, response.c_str(), response.size(), 0);
            }
			exit(EXIT_SUCCESS);
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

			if(Command[i].next == 3){
				waitpid(pid, &status, 0);
			}
			else{
				waitpid(-1, &status, WNOHANG);
			}
		}
	}
}

void pipe_creation(Cmd &cmd){
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

void split_command_line(string line, vector<Cmd> &Command, int &true_line){
    istringstream in(line);
    string s;
	vector<string> args;

    while(in >> s) {
		// Pipe
		if(s[0] == '|' || s[0] == '!'){
			Cmd cmd;
			cmd.argv = args;

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
			else if(args[i] == ">>"){
				cmd.app = true;
				cmd.file = args[i+1];
				args.pop_back();
				args.pop_back();
			}
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

void np_shell(int socket_fd){
    setenv("PATH", "bin:.", 1);
	dup2(socket_fd, STDOUT_FILENO);

    string line(50000, '\0'), sub_line;
    int true_line = 0, number = 0, pos;
    vector<Cmd> c;

    while(1){
        send(socket_fd, "% ", 2, 0);
        int bytes_received = read(socket_fd, &line[0], 49999);
        if(bytes_received == -1){
            err_sys("read message from client error");
        }

        // test environment will different(need to test in np server)
        pos = line.find('\n');
        sub_line = line.substr(0, pos);
        if(sub_line.substr(0, 4) == "exit"){
			close(socket_fd);
			break;
		}
		if(sub_line.empty()){
			continue;
		}
		else if(sub_line.size() >= 6 && sub_line.substr(0, 6) == "setenv"){
			built_in_setenv(sub_line);
			++true_line;
			continue;
		}
		else if(sub_line.size() >= 8 && sub_line.substr(0, 8) == "printenv"){
			built_in_printenv(sub_line, socket_fd);
			++true_line;
			continue;
		}
		else{
			split_command_line(sub_line, c, true_line);
			fork_and_exec_with_pipe(c, socket_fd);
			c.clear();
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


int main(int argc, char* argv[]){
    struct sockaddr_in setaddr;

    int socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(socketfd== -1){
        err_sys("socket creation error");
    }

	int yes=1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
        perror("setsockopt(SO_REUSEADDR) failed");
        return 0;
    }

    setaddr.sin_family = AF_INET;
    setaddr.sin_addr.s_addr = INADDR_ANY;
    setaddr.sin_port = htons(atoi(argv[1]));

    if(bind(socketfd,(const sockaddr*) &setaddr, sizeof(sockaddr_in)) == -1){
        err_sys("socket binding error");
    }

    // backlog :　allow 100 client request connect(if buffer is full, then return error)
    if(listen(socketfd, 1) == -1){
        close(socketfd);
        err_sys("socket listening error");
    }

	while(1){
		int connectfd = accept(socketfd, NULL, NULL);

		if(connectfd == -1){
			close(socketfd);
			err_sys("socket accpetion error");
		}

		// execute npshell
		np_shell(connectfd);
	}

    close(socketfd);
}