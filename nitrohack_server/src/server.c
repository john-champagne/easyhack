/* Copyright (c) Daniel Thaler, 2011. */
/* The NitroHack server may be freely redistributed under the terms of either:
 *  - the NetHack license
 *  - the GNU General Public license v2 or later
 */

/*
 * NitroHack server core.
 * 
 * The code is designed to meet the following goals/constraints:
 *  - Each NitroHack game MUST run in it's own process. The game is the
 *    anti-thesis of thread-safe. Writing a threaded server would require major
 *    work on the game code.
 * 
 *  - Clients should be able to disconnect from the server between commands.
 *    This is required to more easily support web clients. In that scenario
 *    Some javascript in a browser issues an XMLHttpRequest to a web-server,
 *    where a script performs the trivial transformation from request parameters
 *    to a JSON string which it sends on (via localhost) to the NitroHack server.
 *    The NH server responds with some more JSON which represents game state.
 *    Since the request is now complete from their point of view, neither the
 *    web server nor the web client should be required to maintain a connection
 *    to the NH server.
 * 
 * In order to allow a client to reconnect to its running game, there are 2
 * alternatives:
 *   (1) performing each client connection over a new server socket on a
 *       different port.
 *   (2) forwarding the client connection from the master process which owns
 *       the listening socket to a client process which handles the game state.
 * Option (1) has been tried in FTP; this design is widely considered to be a
 * bad idea.
 * 
 * The design of the NH server was based on alternative (2):
 * The server listens for incoming connections.
 * When a connection is made, the client is allowed to send enough data to
 * authenticate.
 * Each authenticated client has its own game process.
 * Communication with this game process is always handled by the main server
 * process: data to and from the client is passed across a set of anonymous
 * pipes.
 * These pipes remain open when the client disconnects from the server; the
 * game process doesn't even notice the disconnection (it may time out from
 * inactivity, though).
 * When the connection is re-established, the client's requests get forwarded
 * again.
 */

#include "nhserver.h"

#include <ctype.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/time.h>

#if defined(OPEN_MAX)
static int get_open_max(void) { return OPEN_MAX; }
#else
static int get_open_max(void) { return sysconf(_SC_OPEN_MAX); }
#endif

/* How many epoll events do we want to process in one go? No idea, actually!
 * 16 seems like a reasonable value for now... */
#define MAX_EVENTS 16

/* a username + password with some fluff should always fit in 500 bytes */
#define AUTH_MAXLEN 500
/* make the buffer slightly bigger to detect when the client sends too much data */
#define AUTHBUFSIZE 512

enum comm_status {
    NEW_CONNECTION,
    CLIENT_DISCONNECTED,
    CLIENT_CONNECTED
};

/* Client communication data.
 * This structure tracks connected clients, ie remote users with open sockets
 * and also "disconnected clients", which refer to local games that continue to
 * run while waiting for the owner to reconnect and issue more commands.
 * 
 * Theoretically the auth info for new sockets could be split out, but it would
 * cause all sorts of headaches and saving a few bytes just isn't worth it. */
struct client_data {
    enum comm_status state;
    int pid;
    int userid; /* owner of this game */
    struct client_data *prev, *next;
    int pipe_out; /* master -> game pipe */
    int pipe_in;/* game -> master pipe */
    int sock; /* master <-> client socket */
    int authdatalen;
    char *authbuf;
};


/*---------------------------------------------------------------------------*/

/* init_list_head: list of connections that have been fully established
 * but not yet authenticated or connected to a game instance.
 * In these sockfd structures, only authbuf, sockfd and status are valid. */
static struct client_data init_list_head;

/* disconnected_list_head: list of games which are fully established, but the
 * client has disconnected. The client can reconnect to the running game later.
 * in these client_data structures, sockfd will be -1, but everything else is valid.*/
static struct client_data disconnected_list_head;

/* connected_list_head: list of games which are fully established and have a
 * connected client. */
static struct client_data connected_list_head;

static struct client_data **fd_to_client;
static int client_count, fd_to_client_max;

/*---------------------------------------------------------------------------*/


static void cleanup_game_process(struct client_data *client, int epfd);


static void link_client_data(struct client_data *client, struct client_data *list)
{
    client->prev = list;
    client->next = list->next;
    list->next = client;
    if (client->next)
	client->next->prev = client;
    
    client_count++;
}

static void unlink_client_data(struct client_data *client)
{
    if (client->prev)
	client->prev->next = client->next;
    if (client->next)
	client->next->prev = client->prev;
    
    client_count--;
}

static struct client_data *alloc_client_data(struct client_data *list_start)
{
    struct client_data *client = malloc(sizeof(struct client_data));
    memset(client, 0, sizeof(struct client_data));
    link_client_data(client, list_start);
    client->state = NEW_CONNECTION;
    client->sock = client->pipe_in = client->pipe_out = -1;
    
    return client;
}


static void map_fd_to_client(int fd, struct client_data *client)
{
    int size;
    
    while (fd >= fd_to_client_max) {
	size = fd_to_client_max * sizeof(struct client_data*);
	fd_to_client = realloc(fd_to_client, 2 * size);
	memset(&fd_to_client[fd_to_client_max], 0, size);
	fd_to_client_max *= 2;
    }
    fd_to_client[fd] = client;
}


/* full setup for both ipv4 and ipv6 server sockets */
static int init_server_socket(struct sockaddr *sa)
{
    int fd, v4, opt_enable = 1;
    
    v4 = sa->sa_family == AF_INET;
    
    fd = socket(sa->sa_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
	log_msg("Error creating server socket: %s", strerror(errno));
	return -1;
    }
    
    /* Enable fast address re-use. Nice to have during development, probably
     * irrelevant otherwise. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(int));
    
    /* Setting the IPV6_V6ONLY socket option allows the ipv6 socket to bind to
     * the same port as the ipv4 socket. Using one socket for each protocol is
     * better than using one ipv4-compatible ipv6 socket, as that limits the
     * possible ipv6 addresses to ipv4 compatible ones. */
    if (!v4 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt_enable, sizeof(int)) == -1)
	log_msg("Failed to set the IPV6_V6ONLY socket option: %s.", strerror(errno));
    
    if (bind(fd, sa, v4 ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) == -1) {
	close(fd);
	log_msg("Error binding server socket (%s): %s", addr2str(sa), strerror(errno));
	return -1;
    }
    
    if (listen(fd, 16) == -1) {
	close(fd);
	log_msg("Error listening on server socket (%s): %s", addr2str(sa), strerror(errno));
	return -1;
    }
    
    return fd;
}


/*
 * Accept a new client connection on one of the listening sockets.
 */
static void server_socket_event(int server_fd, int epfd)
{
    struct epoll_event ev;
    struct client_data *client;
    struct sockaddr_in6 addr; /* sockaddr_in6 is larger than plain sockaddr */
    int newfd, opt_enable = 1;
    socklen_t addrlen = sizeof(addr);
    newfd = accept4(server_fd, (struct sockaddr*)&addr, &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (newfd == -1) /* maybe the connection attempt was aborted early? */
	return;
    
    log_msg("New connection from %s.", addr2str(&addr));
    
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    ev.data.ptr = NULL;
    ev.data.fd = newfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, newfd, &ev) == -1) {
	log_msg("Error in epoll_ctl for %s: %s", addr2str(&addr), strerror(errno));
	close(newfd);
	return;
    }
    
    if (setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY, &opt_enable, sizeof(int)) == -1) {
	log_msg("setting TCP_NODELAY failed: %s", strerror(errno));
    }
    
    client = alloc_client_data(&init_list_head);
    client->sock = newfd;
    map_fd_to_client(newfd, client);
    
    client->authbuf = malloc(AUTHBUFSIZE);
    memset(client->authbuf, 0, AUTHBUFSIZE);
    
    log_msg("There are now %d clients on the server", client_count);
}


/*
 * A new game process is needed.
 * Create the communication pipes, register them with epoll and fork the new
 * process.
 */
static int fork_client(struct client_data *client, int epfd)
{
    int ret1, ret2, i;
    int pipe_out_fd[2];
    int pipe_in_fd[2];
    struct epoll_event ev;
    
    ret1 = pipe2(pipe_out_fd, O_NONBLOCK);
    ret2 = pipe2(pipe_in_fd, O_NONBLOCK);
    if (ret1 == -1 || ret2 == -1) {
	if (!ret1) {
	    close(pipe_out_fd[0]);
	    close(pipe_out_fd[1]);
	}
	/* it's safe to use errno here, even though the second pipe2
	 * call will erase the status from the first, because the
	 * second one will always fail with the same status as the
	 * first if the first call fails. */
	log_msg("Failed to create communication pipes for new connection: %s",
		strerror(errno));
	cleanup_game_process(client, epfd);
	return FALSE;
    }
    
    /* pipe[0] read side - pipe[1] write side */
    fcntl(pipe_in_fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_out_fd[1], F_SETFD, FD_CLOEXEC); /* client does not need to inherit this */
    
    client->pipe_out = pipe_out_fd[1];
    client->pipe_in = pipe_in_fd[0];
    map_fd_to_client(client->pipe_out, client);
    map_fd_to_client(client->pipe_in, client);
    
    client->pid = fork();
    if (client->pid > 0) { /* parent */
    } else if (client->pid == 0) { /* child */
	/* forking doesn't actually close any of the CLOEXEC file
	 * descriptors. CLOEXEC is still nice to have and we can use it as
	 * a flag to get rid of lots of stuff here. */
	for (i = 0; i < get_open_max(); i++)
	    if (fcntl(i, F_GETFD) & FD_CLOEXEC)
		close(i);
	
	/* no auth status to client */
	client_main(client->userid, pipe_out_fd[0], pipe_in_fd[1]);
	exit(0); /* client is done. */
    } else if (client->pid == -1) { /* error */
	/* can't proceed, so clean up. The client side of the pipes needs to be
	 * closed here, this end gets handled in cleanup_game_process */
	close(pipe_out_fd[0]);
	close(pipe_in_fd[1]);
	cleanup_game_process(client, epfd);
	log_msg("Failed to fork a client process: %s", strerror(errno));
	return FALSE;
    }
    
    client->state = CLIENT_CONNECTED;
    unlink_client_data(client);
    link_client_data(client, &connected_list_head);
    
    /* register the pipe fds for monitoring by epoll */
    ev.data.ptr = NULL;
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    ev.data.fd = client->pipe_out;
    epoll_ctl(epfd, EPOLL_CTL_ADD, client->pipe_out, &ev);
    ev.data.fd = client->pipe_in;
    epoll_ctl(epfd, EPOLL_CTL_ADD, client->pipe_in, &ev);
    
    /* close the client side of the pipes */
    close(pipe_out_fd[0]);
    close(pipe_in_fd[1]);
    
    return TRUE;
}


/* 
 * Data is available on client->sock.
 * Now we want to read enough data to authenticate the client. If the client
 * can be authenticated, do so and move the connection into the CLIENT_CONNECTED
 * state. This may involve forking a new game process for the client.
 */
static void handle_new_connection(struct client_data *client, int epfd)
{
    int ret, pos;
    struct client_data *ccur;
    int buflen = AUTHBUFSIZE - client->authdatalen - 1;
    char *bufp = &client->authbuf[client->authdatalen];
    
    ret = read(client->sock, bufp, buflen);
    if (ret == 0) {
	/* socket closed by client (this case should have been caught by epoll
	 * (EPOLLRDHUP), but for safety lets check again. */
	cleanup_game_process(client, epfd);
	return;
    } else if (ret == -1) { /* error? why did epoll give us this fd? */
	log_msg("Error reading socket descriptor that epoll considered ready: %s",
		strerror(errno));
	/* kill it to avoid an infinite loop when epoll reports the same event again. */
	if (errno != EAGAIN)
	    cleanup_game_process(client, epfd);
	return;
    }
    
    client->authdatalen += ret;
    client->authbuf[client->authdatalen] = '\0'; /* make it safe to use as a string */
    
    /* did we receive too much data? */
    if (client->authdatalen >= AUTH_MAXLEN) {
	struct sockaddr_in6 peer; /**/
	socklen_t len = sizeof(peer);
	getpeername(client->sock, (struct sockaddr*)&peer, &len);
	log_msg("Auth buffer overrun attempt from %s? Peer disconnected.", addr2str(&peer));
	cleanup_game_process(client, epfd);
	return;
    }
    
    /* check the end of the received auth data: a JSON object always ends with '}' */
    pos = client->authdatalen - 1;
    while (isspace(client->authbuf[pos]))
	pos--;
    
    if (client->authbuf[pos] != '}') /* not the end of JSON auth data, wait for more */
	return;
    
    /*
     * ready to authenticate the user here
     */
    client->userid = auth_user(client->authbuf);
    if (!client->userid) {
	auth_send_result(client->sock, AUTH_FAILED);
	/* shutdown instead of close here to allow for transmission of the auth status */
	shutdown(client->sock, SHUT_RD);
	/* the socket and client struct will be mopped up when that's done and
	 * epoll reports an EPOLLRDHUP for the socket. */
	return;
    }
    
    /* is the client re-esablishing a connection to an existing, disconnected game? */
    for (ccur = disconnected_list_head.next; ccur; ccur = ccur->next)
	if (ccur->userid == client->userid)
	    break;
    if (ccur) {
	/* there is a running, disconnected game process for this user */
	auth_send_result(client->sock, AUTH_SUCCESS_RECONNECT);
	ccur->sock = client->sock;
	map_fd_to_client(ccur->sock, ccur);
	ccur->state = CLIENT_CONNECTED;
	unlink_client_data(ccur);
	link_client_data(ccur, &connected_list_head);
	
	client->sock = -1; /* sock is now owned by ccur */
	client->pid = 0;
	cleanup_game_process(client, epfd);
	log_msg("Connection to game at pid %d reestablished for user %d",
		ccur->pid, ccur->userid);
	return;
    } else {
	/* there is no process yet */
	if (fork_client(client, epfd))
	    auth_send_result(client->sock, AUTH_SUCCESS_NEW);
    }
}


/*
 * completely free a client_data struct and all its pointers
 */
static void cleanup_game_process(struct client_data *client, int epfd)
{
    /* if the client didn't get a signal yet, send it one now. */
    if (client->pid)
	kill(client->pid, SIGTERM);
    
    /* close all file descriptors and free data structures */
    if (client->sock != -1) {
	epoll_ctl(epfd, EPOLL_CTL_DEL, client->sock, NULL);
	shutdown(client->sock, 0);
	close(client->sock);
	fd_to_client[client->sock] = NULL;
    }
    
    if (client->pipe_out != -1) {
	epoll_ctl(epfd, EPOLL_CTL_DEL, client->pipe_out, NULL);
	close(client->pipe_out);
	fd_to_client[client->pipe_out] = NULL;
    }
    
    if (client->pipe_in != -1) {
	epoll_ctl(epfd, EPOLL_CTL_DEL, client->pipe_in, NULL);
	close(client->pipe_in);
	fd_to_client[client->pipe_in] = NULL;
    }
    
    if (client->authbuf)
	free(client->authbuf);
    
    client->pipe_in = client->pipe_out = client->sock = -1;
    unlink_client_data(client);
    free(client);
    
    log_msg("There are now %d clients on the server", client_count);
}


/*
 * A game process closed one or both of its communication pipes. This only
 * happens when the process is about to exit, because doing so cuts it off from
 * the outside world.
 * This means the server side of both pipes and the client socket can be
 * closed, too.
 */
static void close_client_pipe(struct client_data *client, int epfd)
{
    if (client->pipe_in != -1) {
	epoll_ctl(epfd, EPOLL_CTL_DEL, client->pipe_in, NULL);
	close(client->pipe_in);
	fd_to_client[client->pipe_in] = NULL;
	client->pipe_in = -1;
    }
    
    if (client->pipe_out != -1) {
	epoll_ctl(epfd, EPOLL_CTL_DEL, client->pipe_out, NULL);
	close(client->pipe_out);
	fd_to_client[client->pipe_out] = NULL;
	client->pipe_out = -1;
    }
    
    
    if (client->sock)
	/* allow a send to complete (incl retransmits). close() is too brutal. */
	shutdown(client->sock, SHUT_RD);
    else {
	/* don't try to send a signal in cleanup_game_process - the process may be
	 * gone already */
	client->pid = 0;
	cleanup_game_process(client, epfd);
    }
}


/*
 * handle an epoll event for a fully esablished communication channel, where
 * client->sock, client->pipe_in an client->pipe->out all exist.
 * 
 * The function determines what happened to which file descriptor and passes
 * data around accordingly.
 */
static void handle_communication(int fd, int epfd, unsigned int event_mask)
{
    int ret, closed;
    struct client_data *client = fd_to_client[fd];
    
    if (event_mask & EPOLLERR || /* fd error */
	event_mask & EPOLLHUP || /* fd closed */
	event_mask & EPOLLRDHUP) /* fd closed */
	closed = TRUE;
    else
	closed = FALSE;
    
    if (fd == client->sock) {
	if (closed) { /* peer gone. goodbye. */
	    epoll_ctl(epfd, EPOLL_CTL_DEL, client->sock, NULL);
	    close(client->sock);
	    fd_to_client[client->sock] = NULL;
	    client->sock = -1;
	    if (client->pipe_in != -1 && client->pipe_out != -1) {
		log_msg("User %d has disconnected from a game", client->userid);
		client->state = CLIENT_DISCONNECTED;
		unlink_client_data(client);
		link_client_data(client, &disconnected_list_head);
	    } else {
		log_msg("Shutdown completed for game at pid %d", client->pid);
		client->pid = 0;
		cleanup_game_process(client, epfd);
	    }
		
	    
	} else { /* there is data to receive */
	    do {
		ret = splice(client->sock, NULL, client->pipe_out, NULL,
			     1024 * 1024, SPLICE_F_NONBLOCK);
	    } while (ret == 1024 * 1024);
	    if (ret == -1) {
		log_msg("splice error while receiving: %s", strerror(errno));
		if (errno == EBADF) /* client pipe gone, maybe it crashed? */
		    cleanup_game_process(client, epfd);
	    }
	}

    } else if (fd == client->pipe_in) {
	if (closed)
	    close_client_pipe(client, epfd);

	else { /* there is data to send */
	    /* oddity alert: this code originally used splice for sending.
	     * That would match the receive case above and no buffer would be
	     * required. Unfortunately sending that way is significantly slower.
	     * splice: 200ms - read+write: 0.2ms! Ouch! */
	    char buf[8192];
	    int write_status, errno_orig, write_count;
	    do {
		/* read from the pipe */
		ret = read(client->pipe_in, buf, sizeof(buf));
		errno_orig = errno;

		/* write to the socket; be careful to write all of it... */
		write_count = 0;
		do {
		    write_status = write(client->sock, buf, ret);
		    write_count += write_status;
		} while (write_count < ret && write_status > 0);
	    } while (ret == write_count && ret == sizeof(buf));
	    if (ret == -1)
		log_msg("error while reading from pipe: %s", strerror(errno_orig));
	    if (write_status == -1)
		log_msg("error while sending: %s", strerror(errno));
	}

    } else if (fd == client->pipe_out) {
	if (closed)
	    close_client_pipe(client, epfd);

	else
	    /* closed == FALSE doesn't happen for this fd: it's the write side,
	     * so there should NEVER be anything to read*/
	    log_msg("Impossible: data readable on a write pipe?!?");
    }
}


static int setup_server_sockets(int *ipv4fd, int *ipv6fd, int epfd)
{
    struct epoll_event ev;
    ev.data.ptr = NULL;
    ev.events = EPOLLIN | EPOLLET; /* epoll_wait waits for EPOLLERR and EPOLLHUP as well */
    
    if (!settings.disable_ipv6) {
	settings.bind_addr_6.sin6_port = htons((unsigned short)settings.port);
	*ipv6fd = init_server_socket((struct sockaddr*)&settings.bind_addr_6);
	ev.data.fd = *ipv6fd;
	if (*ipv6fd != -1)
	    epoll_ctl(epfd, EPOLL_CTL_ADD, *ipv6fd, &ev);
    } else
	*ipv6fd = -1;
    
    if (!settings.disable_ipv4) {
	settings.bind_addr_4.sin_port = htons((unsigned short)settings.port);
	*ipv4fd = init_server_socket((struct sockaddr*)&settings.bind_addr_4);
	ev.data.fd = *ipv4fd;
	if (*ipv4fd != -1)
	    epoll_ctl(epfd, EPOLL_CTL_ADD, *ipv4fd, &ev);
    } else
	*ipv4fd = -1;
    
    if (*ipv4fd == -1 && *ipv6fd == -1) {
	log_msg("Failed to create any listening socket. Nothing to do except shut down.");
	return FALSE;
    }
    
    return TRUE;
}


/*
 * The signal handler for SIGTERM and SIGINT ran and set termination_flag to 1
 * to indicate the shutdown request.
 * In response, this function is called from the epoll event loop to close the
 * server sockets and record the shutdown request time.
 */
static int trigger_server_shutdown(struct timeval *tv, int *ipv4fd, int *ipv6fd)
{
    termination_flag = 2;
    
    gettimeofday(tv, NULL);
    log_msg("Shutdown request received; %d clients active.", client_count);
    if (*ipv4fd != -1) {
	close(*ipv4fd);
	*ipv4fd = -1;
    }
    if (*ipv6fd != -1) {
	close(*ipv6fd);
	*ipv6fd = -1;
    }
    if (client_count) {
	log_msg("Server sockets closed, will wait 5 seconds "
		"for clients to shut down.");
	/* because termination_flag is now set, the epoll_wait timeout
	    * will be adjusted based on sigtime */
	return FALSE;
    } else
	return TRUE;
}


/*
 * The server's core. Creates the configured listening sockets and then
 * enters the server event loop from which all clients are served.
 */
int runserver(void)
{
    int i, ipv4fd, ipv6fd, epfd, nfds, timeout, fd;
    struct epoll_event events[MAX_EVENTS];
    struct client_data *client;
    struct timeval sigtime, curtime, tmp;
    
    fd_to_client_max = 64; /* will be doubled every time it becomes too small */
    fd_to_client = malloc(fd_to_client_max * sizeof(struct client_data*));
    
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
	log_msg("Error in epoll_create1");
	return FALSE;
    }
    
    if (!setup_server_sockets(&ipv4fd, &ipv6fd, epfd))
	return FALSE;
    
    /*
     * server event loop
     */
    while (1) { /* loop exit via "goto finally" */
	timeout = 10 * 60 * 1000;
	if (termination_flag) {
	    if (termination_flag == 1) /* signal didn't interrupt epoll_wait */
		trigger_server_shutdown(&sigtime, &ipv4fd, &ipv6fd);
	    gettimeofday(&curtime, NULL);
	    /* calculate the elapsed time since the quit request */
	    timersub(&curtime, &sigtime, &tmp);
	    timeout = 5000 - (1000 * tmp.tv_sec) - (tmp.tv_usec / 1000);
	    if (timeout < 0)
		timeout = 0;
	}
	
	nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
	if (nfds == -1) {
	    if (errno != EINTR) { /* serious problem */
		log_msg("Error from epoll_wait in main event loop: %s", strerror(errno));
		goto finally;
	    }
	    /* some signal - SIGTERM? */
	    if (!termination_flag)
		continue;
	    
	    /* begin server shutdown sequence */
	    if (!trigger_server_shutdown(&sigtime, &ipv4fd, &ipv6fd))
		continue;
	    else
		goto finally;
	}
	else if (nfds == 0) { /* timeout */
	    if (!termination_flag)
		log_msg(" -- mark (no activity for 10 minutes) --");
	    else /* shutdown timer has run out */
		goto finally;
	    continue;
	}

	for (i = 0; i < nfds; i++) {
	    fd = events[i].data.fd;
	    
	    if (fd == ipv4fd || fd == ipv6fd) { /* server socket ready for accept */
		server_socket_event(fd, epfd);
		continue;
	    }
	    
	    /* activity on a client socket or pipe */
	    client = fd_to_client[fd];
	    /* was this fd closed while handling a prior event? */
	    if (!client)
		continue;
	    
	    switch (client->state) {
		case NEW_CONNECTION:
		    if (events[i].events & EPOLLERR || /* error */
			events[i].events & EPOLLHUP || /* connection closed */
			events[i].events & EPOLLRDHUP) /* connection closed */
			cleanup_game_process(client, epfd);
		    else if (events[i].events & EPOLLIN)
			handle_new_connection(client, epfd);
		    else {
			log_msg("Unexpected event type %08x on new client socket???",
				events[i].events);
			cleanup_game_process(client, epfd);
		    }
		    break;
		    
		case CLIENT_DISCONNECTED:
		    /* When the client is disconnected, activity usually only
		     * happens on the pipes: either the game process is
		     * closing them because the idle timeout expired or
		     * shutdown was requested via a signal.
		     */
		    if (events[i].events & EPOLLERR || /* error */
			events[i].events & EPOLLHUP || /* connection closed */
			events[i].events & EPOLLRDHUP) /* connection closed */
			cleanup_game_process(client, epfd);
		    else {
			/* Perhaps the game process was just writing data to
			 * the pipe, when the client disconnected.
			 * There is nothing we can do with this data here, but
			 * we don't want to kill the game either, so just read
			 * and discard the data. */
			char buf[2048];
			int ret;
			do {
			    ret = read(fd, buf, 2048);
			} while (ret == 2048);
		    }
		    break;
		    
		case CLIENT_CONNECTED:
		    handle_communication(fd, epfd, events[i].events);
		    break;
	    }
	    if (termination_flag && client_count == 0)
		goto finally;
	} /* for */
    } /* while(1) */

finally:
    while (init_list_head.next)
	cleanup_game_process(init_list_head.next, epfd);
    while (disconnected_list_head.next)
	cleanup_game_process(disconnected_list_head.next, epfd);
    while (connected_list_head.next)
	cleanup_game_process(connected_list_head.next, epfd);

    close(epfd);
    if (ipv4fd > 0)
	close(ipv4fd);
    if (ipv6fd > 0)
	close(ipv6fd);

    return TRUE;
}

/* server.c */