#include <domain_sockets.h>
#include <ns_limits.h>
#include <poll_helpers.h>

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <assert.h>
#include <poll.h>
#include <stdlib.h>

#define MAX_BUFFER 1024

/* Whatever information you need to save for a server (name, pipes, pid, etc...) */
struct server_info {
   char *name;
   char *binary;
   int sockfd;
   int pid;
   int pipe_w[2];
   int pipe_r[2];
};

/* All of our servers */
struct server_info servers[MAX_SRV];
int num_srvs =0;

/* Each file descriptor can be used to lookup the server it is associated with here */
struct server_info *client_fds[MAX_FDS];

/* The array of pollfds we want events for; passed to poll */
struct pollfd poll_fds[MAX_FDS];
int num_fds = 0;

/*
* If you want to use these functions to add and remove descriptors
* from the `poll_fds`, feel free! If you'd prefer to use the logic
* from the lab, feel free!
*/
void poll_create_fd(int fd)
{
   assert(poll_fds[num_fds].fd == 0);
   poll_fds[num_fds] = (struct pollfd){
      .fd = fd,
      .events = POLLIN
   };
   num_fds++;
}

void poll_remove_fd(int fd)
{
   int i;
   struct pollfd *pfd = NULL;
   
   assert(fd != 0);
   for (i = 0; i < num_fds; i++)
   {
      if (fd == poll_fds[i].fd)
      {
         pfd = &poll_fds[i];
         break;
      }
   }
   
   if (pfd == NULL) {
      // File descriptor not found in the array
      return;
   }
   
   close(fd);
   
   /* Replace the fd by copying the last one to fill the gap */
   *pfd = poll_fds[num_fds - 1];
   poll_fds[num_fds - 1].fd = 0;
   num_fds--;
}

/*
* Create a new server identified by `name` and backed by binary `binary`.
*
* Returns 0 on success, -1 on failure.
*/
int server_create(char *name, char *binary)
{
   struct server_info * ptr = &servers[num_srvs];

    ptr->name = name;
    ptr->binary = binary;

    // Create pipes for communication
    if (pipe(ptr->pipe_r) < 0) {
        panic("pipe error");
    }

    if (pipe(ptr->pipe_w) < 0) {
        panic("pipe error");
    }

    // Fork to create a new process
    ptr->pid = fork();
    if (ptr->pid < 0) {
        panic("FORK ERROR");
    }
    else if (ptr->pid == 0) {
        char *args[] = {binary, NULL};
        dup2(ptr->pipe_r[0], STDIN_FILENO);
        dup2(ptr->pipe_w[1], STDOUT_FILENO);
        close(ptr->pipe_r[0]);
        close(ptr->pipe_r[1]);
        close(ptr->pipe_w[0]);
        close(ptr->pipe_w[1]);
        execvp(args[0], args);
    }
    close(ptr->pipe_w[1]);
    close(ptr->pipe_r[0]);

    // Create a domain socket for the server
    ptr->sockfd = domain_socket_server_create(name);

    if (ptr->sockfd < 0) {
        unlink(name);
        ptr->sockfd = domain_socket_server_create(name);
        if (ptr->sockfd < 0) {
            panic("SOCKET ERROR");
        }
    }
    poll_create_fd(ptr->sockfd);
    num_srvs++;
    return 0;
}

int main(int argc, char *argv[])
{
   ignore_sigpipe();

    // Check command line arguments
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <name_server_map_file>\n", argv[0]);
        fprintf(stderr, "Usage: %s <domain_socket_name>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    // Read server map and create servers
    if (read_server_map(argv[1], server_create)) {
        panic("Configuration file error");
    }
    /* The event loop */
    while (1) {
        int ret;
        int i;
        struct server_info* ptr;
        char buffer[1024];
        char buffer2[1024];
        int new_client;
        // Poll for events
        ret = poll(poll_fds, num_fds, -1);
        if (ret <= 0) {
            panic("poll error");
        }
        // Check for new clients on server sockets
        for (i = 0; i < num_srvs; i++) {
            ptr = &servers[i];
            if (poll_fds[i].revents == POLLIN) {
                if ((new_client = accept(ptr->sockfd, NULL, NULL)) == -1) {
                    panic("ACCEPT ERROR");
                }
                poll_create_fd(new_client);
                client_fds[new_client] = ptr;
                poll_fds[num_fds].revents = 0;
            }
        }
        // Handle data from clients
        for (i = num_srvs; i < num_fds; i++) {
            ptr = client_fds[poll_fds[i].fd];
            if (poll_fds[i].revents == POLLIN) {
                int j;
                poll_fds[i].revents = 0;
                memset(buffer, 0, 1024);
                memset(buffer2, 0, 1024);
                j = read(poll_fds[i].fd, buffer, 1024);
                if (j == -1) {
                    if (errno == EPIPE) {
                        continue;
                    }
                    panic("READ ERROR");
                }
                buffer[j] = '\0';
                if (j == 0) {
                    panic("ZERO READ ERROR");
                }
                write(ptr->pipe_r[1], buffer, strlen(buffer));
                j = read(ptr->pipe_w[0], buffer2, 1024);
                if (j == -1) {
                    panic("READ ERROR");
                }
                buffer2[j] = '\0';
                write(poll_fds[i].fd, buffer2, j);
            }
            if (poll_fds[i].revents & (POLLHUP | POLLERR)) {
                poll_fds[i].revents = 0;
                poll_remove_fd(poll_fds[i].fd);
                i--;
                continue;
            }
        }
    }
    return 0;
}
