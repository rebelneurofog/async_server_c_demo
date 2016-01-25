#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>

#define SERVICE_PORT 4455
#define SPAM_PERIOD_SEC 5 /* Broadcast a message each N seconds */

typedef struct application_t {
    int listener_fd;
    int max_connections;
    int connection_count;
    int *connection_fds;
} application_t;

static application_t *application_create (int port, int max_connections)
{
    int listener_fd = socket (AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
	.sin_family = AF_INET,
	.sin_port = htons ((uint16_t) port),
	.sin_addr = {.s_addr = 0 /* To listen to 0.0.0.0 */},
    };
    if (listener_fd >= 0) {
	int on_flag = 1;
	setsockopt (listener_fd, SOL_SOCKET, SO_REUSEADDR, &on_flag, sizeof (on_flag));
	if (bind (listener_fd, (struct sockaddr*) &addr, sizeof (struct sockaddr_in)) != -1) {
	    if (fcntl (listener_fd, F_SETFL, fcntl (listener_fd, F_GETFL, 0) | O_NONBLOCK) != -1) {
		if (listen (listener_fd, 256) != -1) {
		    fprintf (stderr, "Listening at '0.0.0.0:%d'\n", port);
		    application_t *s = malloc (sizeof (application_t) + sizeof (int)*max_connections);
		    s->listener_fd = listener_fd;
		    s->max_connections = max_connections;
		    s->connection_count = 0;
		    s->connection_fds = (void*) (s + 1);
		    return s;
		} else {
		    fprintf (stderr, "Failed to start listening for incoming connections: %s\n", strerror (errno));
		}
	    } else {
		fprintf (stderr, "Failed to put socket into non-blocking mode: %s\n", strerror (errno));
	    }
	} else {
	    fprintf (stderr, "Failed to bind socket to address '0.0.0.0:%d': %s\n", port, strerror (errno));
	}
    } else {
	fprintf (stderr, "Failed to create socket: %s\n", strerror (errno));
    }
    return NULL;
}
static void application_destroy (application_t *s)
{
    if (!s) return;
    close (s->listener_fd);
    free (s);
}
static void application_add_connection (application_t *s, int connection_fd)
{
    if (s->connection_count == s->max_connections) {
	fprintf (stderr, "Connection limit of %d reached, dropping new connection\n", s->max_connections);
	close (connection_fd);
	return;
    }
    if (fcntl (connection_fd, F_SETFL, fcntl (connection_fd, F_GETFL, 0) | O_NONBLOCK) == -1) {
	fprintf (stderr, "Failed to put socket into non-blocking mode: %s\n", strerror (errno));
	close (connection_fd);
	return;
    }
    s->connection_fds[s->connection_count++] = connection_fd;
}
static void application_remove_connection (application_t *s, int connection_index)
{
    close (s->connection_fds[connection_index]);
    memmove (s->connection_fds + connection_index, s->connection_fds + (connection_index + 1), sizeof (int)*(s->connection_count - connection_index - 1));
    --(s->connection_count);
}
static int timespec_gt (const struct timespec *time1, const struct timespec *time2) /* > */
{
    return (time1->tv_sec > time2->tv_sec) || ((time1->tv_sec == time2->tv_sec) && (time1->tv_nsec > time2->tv_nsec));
}
static void timespec_sub (struct timespec *dtime, const struct timespec *time1, const struct timespec *time2) /* - */
{
    if (time1->tv_sec > time2->tv_sec) {
	if (time1->tv_nsec >= time2->tv_nsec) {
	    dtime->tv_sec = time1->tv_sec - time2->tv_sec;
	    dtime->tv_nsec = time1->tv_nsec - time2->tv_nsec;
	} else {
	    dtime->tv_sec = time1->tv_sec - time2->tv_sec - 1;
	    dtime->tv_nsec = time1->tv_nsec + 1000000000 - time2->tv_nsec;
	}
    } else if ((time1->tv_sec == time2->tv_sec) && (time1->tv_nsec >= time2->tv_nsec)) {
	dtime->tv_sec = time1->tv_sec - time2->tv_sec;
	dtime->tv_nsec = time1->tv_nsec - time2->tv_nsec;
    } else {
	dtime->tv_sec = 0;
	dtime->tv_nsec = 0;
    }
}
static int application_main_loop (application_t *s)
{
    struct timespec initial_time;
    if (clock_gettime (CLOCK_MONOTONIC, &initial_time) < 0) {
	fprintf (stderr, "Failed to get time: %s\n", strerror (errno));
	return 1;
    }
    struct timespec messaging_time = initial_time;
    ++(messaging_time.tv_sec);
    int tick_n = 1;
    while (1) {
	fd_set selectable_fds;
	int top_fd;
	{
	    /* Preparing FDs for pselect () */
	    FD_ZERO (&selectable_fds);
	    FD_SET (s->listener_fd, &selectable_fds);
	    top_fd = s->listener_fd;
	    int i;
	    for (i = 0; i < s->connection_count; ++i) {
		int fd = s->connection_fds[i];
		FD_SET (fd, &selectable_fds);
		if (fd > top_fd)
		    top_fd = fd;
	    }
	}
	struct timespec timeout; {
	    struct timespec current_time;
	    if (clock_gettime (CLOCK_MONOTONIC, &current_time) < 0) {
		fprintf (stderr, "Failed to get time: %s\n", strerror (errno));
		return 1;
	    }
	    timespec_sub (&timeout, &messaging_time, &current_time);
	}
	int pselect_ret = pselect (top_fd + 1, &selectable_fds, NULL, NULL, &timeout, NULL);
	if (pselect_ret > 0) { /* Have things to do */
	    int connection_count = s->connection_count; /* We need the count at this current state since we won't handle newly established connections yet */
	    if (FD_ISSET (s->listener_fd, &selectable_fds)) { /* Accepting incoming connections */
		struct sockaddr_in addr;
		socklen_t addr_len = sizeof (struct sockaddr_in);
		int connection_fd;
		while ((connection_fd = accept (s->listener_fd, (struct sockaddr*) &addr, &addr_len)) >= 0) {
		    uint32_t ip_address = ntohl (addr.sin_addr.s_addr);
		    fprintf (stderr, "Have incoming connection from '%d.%d.%d.%d:%d'\n",
			     (int) ((ip_address >> 24) & 0xff),
			     (int) ((ip_address >> 16) & 0xff),
			     (int) ((ip_address >> 8) & 0xff),
			     (int) ((ip_address >> 0) & 0xff),
			     (int) ntohs (addr.sin_port));
		    application_add_connection (s, connection_fd);
		}
		if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
		    fprintf (stderr, "accept () returned with error: %s\n", strerror (errno));
		    return 1;
		}
	    }
	    /* Handling established connections */
	    int i;
	    for (i = 0; i < connection_count;) {
		int fd = s->connection_fds[i];
		if (FD_ISSET (fd, &selectable_fds)) {
		    char buffer[4096];
		    ssize_t len;
		    while ((len = read (fd, buffer, sizeof (buffer))) > 0) {
			struct timespec current_time;
			if (clock_gettime (CLOCK_MONOTONIC, &current_time) < 0) {
			    fprintf (stderr, "Failed to get time: %s\n", strerror (errno));
			    return 1;
			}
			/* We could have buffered this but we don't do so for simplicity */
			char prefix[256];
			sprintf (prefix, "Message from socket %d at %d.%09d: ", fd, (int) current_time.tv_sec, (int) current_time.tv_nsec);
			struct iovec iov[3] = { /* To avoid additional copying */
			    {
				.iov_base = prefix,
				.iov_len = strlen (prefix),
			    },
			    {
				.iov_base = buffer,
				.iov_len = len,
			    },
			    {
				.iov_base = "\n",
				.iov_len = 1,
			    },
			};
			int j;
			for (j = 0; j < s->connection_count; ++j) {
			    /* So without buffering we only rely on kernel buffer: if it is full, the data won't be written and will simply be dropped */
			    if (writev (s->connection_fds[j], iov, 3) < 0)
				fprintf (stderr, "Failed to write to socket (doing nothing about it): %s\n", strerror (errno));
			}
		    }
		    if (!len) { /* End Of File (connection closed on client side) */
			application_remove_connection (s, i);
			--connection_count;
		    } else if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
			fprintf (stderr, "Failed to read from socket: %s\n", strerror (errno));
			application_remove_connection (s, i);
			--connection_count;
		    } else {
			++i;
		    }
		} else {
		    ++i;
		}
	    }
	} else if (pselect_ret == -1) {
	    fprintf (stderr, "pselect () returned with error: %s\n", strerror (errno));
	    return 1;
	}
	/* Handling time-scheduled quests */
	struct timespec current_time;
	if (clock_gettime (CLOCK_MONOTONIC, &current_time) < 0) {
	    fprintf (stderr, "Failed to get time: %s\n", strerror (errno));
	    return 1;
	}
	if (timespec_gt (&current_time, &messaging_time)) {
	    char message[256];
	    sprintf (message, "Tick %d\n", tick_n);
	    ++tick_n;
	    int j;
	    for (j = 0; j < s->connection_count; ++j) {
		if (write (s->connection_fds[j], message, strlen (message)) < 0)
		    fprintf (stderr, "Failed to write to socket (doing nothing about it): %s\n", strerror (errno));
	    }
	    while (timespec_gt (&current_time, &messaging_time))
		messaging_time.tv_sec += SPAM_PERIOD_SEC;
	}
    }
    return 0;
}

int main ()
{
    application_t *application = application_create (SERVICE_PORT, 1024);
    if (!application)
	return 1;
    int ret_code = application_main_loop (application);
    application_destroy (application);
 
    return ret_code;
}
