#include "UserTerminalHandler.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#else
#include <pty.h>
#endif

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
#include "UserTerminalRouter.hpp"

#include "ETerminal.pb.h"

namespace et {
UserTerminalHandler::UserTerminalHandler() {}

void UserTerminalHandler::connectToRouter(const string &idPasskey) {
  sockaddr_un remote;

  routerFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(routerFd);
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, ROUTER_FIFO_NAME);

  if (connect(routerFd, (struct sockaddr *)&remote, sizeof(sockaddr_un)) < 0) {
    close(routerFd);
    if (errno == ECONNREFUSED) {
      cout << "Error:  The Eternal Terminal daemon is not running.  Please "
              "(re)start the et daemon on the server."
           << endl;
    } else {
      cout << "Error:  Connection error communicating with et deamon: "
           << strerror(errno) << "." << endl;
    }
    exit(1);
  }
  FATAL_FAIL(writeAll(routerFd, &(idPasskey[0]), idPasskey.length()));
  FATAL_FAIL(writeAll(routerFd, "\0", 1));
}

void UserTerminalHandler::run(string jumpcmd) {
  int masterfd;
  if (jumpcmd.empty()) {
    // this is dst, open a psuedo-terminal.
    pid_t pid = forkpty(&masterfd, NULL, NULL, NULL);
    switch (pid) {
      case -1:
        FATAL_FAIL(pid);
      case 0: {
        close(routerFd);
        passwd *pwd = getpwuid(getuid());
        chdir(pwd->pw_dir);
        string terminal = string(::getenv("SHELL"));
        VLOG(1) << "Child process " << pid << " launching terminal " << terminal
                << endl;
        setenv("ET_VERSION", ET_VERSION, 1);
        execl(terminal.c_str(), terminal.c_str(), "--login", NULL);
        exit(0);
        break;
      }
      default: {
        // parent
        VLOG(1) << "pty opened " << masterfd << endl;
        runUserTerminal(masterfd, pid);
        close(routerFd);
        break;
      }
    }
  } else {
    // this is a jumphost, start etclient with all cmd options to connect to dst.
    int writepipe[2] = {-1, -1}, /* parent -> child */
        readpipe[2] = {-1, -1}; /* child -> parent */
    writepipe[0] = -1;

    if (pipe(readpipe) < 0 || pipe(writepipe) < 0) {
	    cout << "pipe" << endl;
	    exit(1);
    }
    pid_t childpid = fork();
    cout << "Ailing: before" << endl;
    switch(childpid) {
	    case -1:
	      FATAL_FAIL(childpid);
	    case 0: {
		close(writepipe[1]);// parent write
		close(readpipe[0]); //parent read
		dup2(writepipe[0], 0);// child read -> stdin 
		close(writepipe[0]);
		dup2(readpipe[1], 1);// child write -> stdout
		close(readpipe[1]);
		//do child
                try{
		  //TODO: automate this 
		  string et_path = "/Users/ailzhang/dev/EternalTCP/build/etclient";
		  auto options = split(jumpcmd, '#');
		  int num_options = options.size();

		  char *paramList[2*(num_options+1)];
		  paramList[0] = (char *)et_path.c_str();
		  int i = 1;
		  for (auto op: options){
			auto values = split(op, '=');
			string op_name = values[0];
		       string op_value = values[1];
		       op_value.erase(op_value.find_last_not_of(" \n\r\t") + 1);
		       paramList[i] = new char[sizeof(op_name)+1];
		       strcpy(paramList[i], op_name.c_str()); 
		       paramList[i+1] = new char[sizeof(op_value)+1];
		       strcpy(paramList[i+1], op_value.c_str()); 
		       i += 2;
	       	  }	       
		  paramList[i] = NULL;
                  string ETCLIENT_BINARY = "etclient";
		  string terminal = string(::getenv("SHELL"));
		  // for testing only
		  // use the format that current etclient accepts.
		  char *param[] = {"/Users/ailzhang/dev/EternalTCP/build/etclient", "--host", "devvm26048.prn1.facebook.com", "--port", "8080", NULL};
		  for (char *p: param) {
			  if(p != NULL)
			  cout << string(p).length() << endl;
		  }
                  execv(et_path.c_str(), paramList);
		  // Simple command below to test pipes between child and parent.
		  // execl("/bin/cat","/bin/cat", "/Users/ailzhang/dev/EternalTCP/build/hello", NULL); 
		  cout << "execv error" << endl;
		  break;
                } catch (const runtime_error& err) {
			cout << "etclient error" << endl;
                  LOG(ERROR) << "Error setting up connection from jumphost to dst" << endl;
                  exit(1);
                }
	    }
	    default: {
		//do parent
		VLOG(1) << "jumphost created " << endl;
		close(writepipe[0]); // child read
		close(readpipe[1]); // child write
		runJumphost(readpipe[0], writepipe[1], childpid);
		close(readpipe[0]);
		close(writepipe[1]);
		break;
	     }
    }
  }
}

void UserTerminalHandler::runUserTerminal(int masterFd, pid_t childPid) {
#ifdef WITH_UTEMPTER
  {
    char buf[1024];
    sprintf(buf, "et [%lld]", (long long)getpid());
    utempter_add_record(masterFd, buf);
  }
#endif

  bool run = true;

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  while (run) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(masterFd, &rfd);
    FD_SET(routerFd, &rfd);
    int maxfd = max(masterFd, routerFd);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);
    // Check for data to receive; the received
    // data includes also the data previously sent
    // on the same master descriptor (line 90).
    if (FD_ISSET(masterFd, &rfd)) {
      // Read from terminal and write to client
      memset(b, 0, BUF_SIZE);
      int rc = read(masterFd, b, BUF_SIZE);
      FATAL_FAIL(rc);
      if (rc > 0) {
        writeAll(routerFd, b, rc);
      } else {
        LOG(INFO) << "Terminal session ended";
        siginfo_t childInfo;
        FATAL_FAIL(waitid(P_PID, childPid, &childInfo, WEXITED));
        run = false;
        break;
      }
    }

    try {
      if (FD_ISSET(routerFd, &rfd)) {
        char packetType;
        int rc = read(routerFd, &packetType, 1);
        FATAL_FAIL(rc);
        if (rc == 0) {
          throw std::runtime_error(
              "Router has ended abruptly.  Killing terminal session.");
        }
        switch (packetType) {
          case TERMINAL_BUFFER: {
            TerminalBuffer tb = readProto<TerminalBuffer>(routerFd);
            const string &buffer = tb.buffer();
            FATAL_FAIL(writeAll(masterFd, &buffer[0], buffer.length()));
            break;
          }
          case TERMINAL_INFO: {
            TerminalInfo ti = readProto<TerminalInfo>(routerFd);
            winsize tmpwin;
            tmpwin.ws_row = ti.row();
            tmpwin.ws_col = ti.column();
            tmpwin.ws_xpixel = ti.width();
            tmpwin.ws_ypixel = ti.height();
            ioctl(masterFd, TIOCSWINSZ, &tmpwin);
            break;
          }
        }
      }
    } catch (std::exception ex) {
      LOG(INFO) << ex.what();
      run = false;
      break;
    }
  }

#ifdef WITH_UTEMPTER
  utempter_remove_record(masterFd);
#endif
}

void UserTerminalHandler::runJumphost(int readFd, int writeFd, pid_t childPid) {
  bool run = true;
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];
  char c[BUF_SIZE];
  
  while (run) {
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(readFd, &rfd);
    FD_SET(routerFd, &rfd);
    int maxfd = max(readFd, routerFd);
    tv.tv_sec = 10;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    if (FD_ISSET(readFd, &rfd)) {
      // Read from terminal and write to client
      memset(b, 0, BUF_SIZE);
      int rc = read(readFd, b, BUF_SIZE);
      cout << "start/" << b << "/end" << endl;
      FATAL_FAIL(rc);
      if (rc > 0) {
        writeAll(routerFd, b, rc);
      } else {
        LOG(INFO) << "Terminal session ended";
        siginfo_t childInfo;
        FATAL_FAIL(waitid(P_PID, childPid, &childInfo, WEXITED));
        run = false;
        break;
      }
    }

    try {
      if (FD_ISSET(routerFd, &rfd)) {
	memset(c, 0, BUF_SIZE);
	int rc = read(routerFd, c, BUF_SIZE);
	cout << "router start/:" << c << "/router end" << endl;
	FATAL_FAIL(rc);
	if (rc > 0 ) {
	  writeAll(writeFd, c, rc);
	} else {
	  LOG(INFO) << "Router session ended";
	  run = false;
	  break;
	}
      }
    } catch (std::exception ex) {
      LOG(INFO) << ex.what();
      run = false;
      break;
    }
  }
}

}  // namespace et
