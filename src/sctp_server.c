/**
 * @file sctp_server.c Simple SCTP server 
 *
 * Copyright (c) 2009 - 2010, J. Taimisto <jtaimisto@gmail.com>
 * All rights reserved.
 *  
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: 
 *
 *     - Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 *     - Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>

#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#include "defs.h"
#include "debug.h"
#include "common.h"

#define DEFAULT_PORT 2001
#define DEFAULT_BACKLOG 2

#define RECVBUF_SIZE 1024

#define PROG_VERSION "0.0.2"

/* Operation flags */
#define VERBOSE_FLAG 0x01
#define SEQPKT_FLAG 0x01 <<1 
#define ECHO_FLAG 0x01 <<2
/**
 * Number of milliseconds to wait on select() before checking if user has
 * requested stop.
 */
#define ACCEPT_TIMEOUT_MS 100


/**
 * Indication that user has requested close
 */
static int close_req = 0;

/**
 * The main context.
 */
struct server_ctx {
        int sock; /**< Socket we are listening for connections */
        uint16_t port; /**< Port we are listening on */
        uint8_t *recvbuf; /**< Buffer where data is received */
        uint16_t recvbuf_size; /**< Number of bytes of data on buffer */
        flags_t options;/**< Operation flags */
        
};


/**
 * Bind to requested port and set the socket to listen for incoming
 * connections.
 * The port is read from the main context.
 *
 * @param ctx Pointer to the main context.
 * @return -1 if operation failed, 0 on success.
 */
int bind_and_listen( struct server_ctx *ctx )
{
        struct sockaddr_in6 ss;

        DBG("Binding to port %d \n", ctx->port );
        memset( &ss, 0, sizeof( ss ));
        ss.sin6_family = AF_INET6;

        ss.sin6_port = htons(ctx->port);

        memcpy( &ss.sin6_addr, &in6addr_any, sizeof(struct in6_addr));
        if ( bind(ctx->sock, (struct sockaddr *)&ss, sizeof( struct sockaddr_in6)) < 0 ) {
                ERROR( "Unable to bind() : %s \n", strerror(errno));
                return -1;
        }

        if ( listen( ctx->sock, DEFAULT_BACKLOG ) < 0 ) {
                ERROR(" Unable to listen() : %s \n", strerror(errno));
                return -1;
        }

        return 0;
}


/**
 * Wait for incoming connection.
 *
 * Returns either when a connection from remote server accepted or if stop_req
 * is set to 1.
 *
 * @param ctx Pointer to main context.
 * @param remote_ss Address of the remote host is saved here.
 * @param addrlen Pointer to variable where lenght of remote host data is saved.
 * @return -1 on error, the fd for accepted connection on success.
 */
int do_accept( struct server_ctx *ctx, struct sockaddr_storage *remote_ss, 
                socklen_t *addrlen )
{
        int cli_fd, ret = 0;
        fd_set fds;
        struct timeval tv;

        while( ret == 0 ) {
                if ( close_req )
                        return 0;

                FD_ZERO( &fds );
                FD_SET( ctx->sock, &fds );

                memset( &tv, 0, sizeof(tv));

                tv.tv_usec = ACCEPT_TIMEOUT_MS * 1000;

                ret = select( ctx->sock+1, &fds, NULL, NULL, &tv );
                if ( ret > 0 && FD_ISSET( ctx->sock, &fds ) )  {
                        TRACE("Going to accept()\n");
                        cli_fd = accept( ctx->sock, (struct sockaddr *)remote_ss, 
                                        addrlen );
                        if ( cli_fd < 0 ) {
                                if ( errno == EINTR ) 
                                        continue; /* likely we are closing */

                                fprintf(stderr, "Error in accept() : %s \n", strerror(errno));
                                return -1;
                        }
                } else if ( ret < 0 ) {
                        if ( errno == EINTR ) 
                                continue;

                        fprintf( stderr, "Error in select() : %s \n", strerror(errno));
                        return -1;
                }
        }
        return cli_fd;
}

/**
 * Server loop when STREAM socket is used. 
 *
 * Wait for incoming data from remote peer and if echo mode is on, echo it
 * back.
 * 
 * @param ctx Pointer to main context.
 * @param client_fd Socket to read the data from. 
 * @return -1 if error occurs, 0 on success.
 */
int do_server( struct server_ctx *ctx, int client_fd )
{
        int recv_count = 1;

        while ( ! close_req ) {

                recv_count = recv_wait( client_fd, ACCEPT_TIMEOUT_MS,
                                ctx->recvbuf, ctx->recvbuf_size, 
                                NULL, NULL, NULL );
                
                if ( recv_count == -1 ) {
                        if ( errno == EINTR )
                                continue;

                        ERROR("Error in recv_wait() : %s \n", strerror(errno));
                        return -1;
                } else if ( recv_count == -2 ) {
                        printf("Connection closed by the remote host\n");
                        return 0;
                } else if ( recv_count > 0 )  {
                        DBG("Received %d bvtes \n", recv_count );
                        xdump_data( stdout, ctx->recvbuf, recv_count, "Received data");
                        if ( is_flag( ctx->options, ECHO_FLAG ) ) {
                                DBG("Echoing data back\n");
                                if ( send( client_fd, ctx->recvbuf, recv_count, 0 ) < 0 ) {
                                        WARN("send() failed while echoing received data\n");
                                }
                        }
                }
        }
        return 0;
}

/**
 * Server loop when SEQPKT socket is used. 
 *
 * Wait for incoming data from remote peer and if echo mode is on, echo it
 * back.
 * 
 * @param ctx Pointer to main context.
 * @return -1 if error occurs, 0 on success.
 */
int do_server_seq( struct server_ctx *ctx ) 
{
        struct sockaddr_storage peer_ss;
        socklen_t peerlen;
        struct sctp_sndrcvinfo info;
        int ret;
        char peername[INET6_ADDRSTRLEN];
        void *ptr;
        uint16_t port;


        while( ! close_req ) {

                memset( &peer_ss, 0, sizeof( peer_ss ));
                memset( &info, 0, sizeof( peer_ss ));
                peerlen = sizeof( struct sockaddr_in6);

                ret = recv_wait( ctx->sock, ACCEPT_TIMEOUT_MS,
                                ctx->recvbuf, ctx->recvbuf_size, 
                                (struct sockaddr *)&peer_ss, &peerlen,
                                &info );
                if ( ret == -1 ) {
                        if ( errno == EINTR )
                                continue;

                        ERROR("Error in recv_wait() : %s \n", strerror( errno ));
                        return -1;
                } else if ( ret == -2 )  {
                        printf("Connection closed by remote host\n" );
                } else if ( ret > 0 ) {
                        DBG("Received %d bytes \n", ret );
                        if ( peer_ss.ss_family == AF_INET ) {
                                ptr = &(((struct sockaddr_in *)&peer_ss)->sin_addr);
                                port = ((struct sockaddr_in *)&peer_ss)->sin_port;
                        } else {
                                ptr = &(((struct sockaddr_in6 *)&peer_ss)->sin6_addr);
                                port = ((struct sockaddr_in6 *)&peer_ss)->sin6_port;
                        }
                        if ( inet_ntop(peer_ss.ss_family, ptr, peername, peerlen ) != NULL ) {
                                printf("Packet from %s:%d ", peername, ntohs(port));
                        } else {
                                printf("Packet from unknown");
                        }
                        printf(" with %d bytes of data\n", ret);
                        if ( is_flag( ctx->options, VERBOSE_FLAG ) ) {
                                printf("\t stream: %d ppid: %d context: %d\n", info.sinfo_stream, 
                                                info.sinfo_ppid, info.sinfo_context );
                                printf("\t ssn: %d tsn: %u cumtsn: %u ", info.sinfo_ssn, 
                                                info.sinfo_tsn, info.sinfo_cumtsn );
                                printf("[");
                                if ( info.sinfo_flags & SCTP_UNORDERED ) 
                                  printf("un");
                                printf("ordered]\n");

                                  
                        }
                        xdump_data( stdout, ctx->recvbuf, ret, "Received data" );
                        if ( is_flag( ctx->options, ECHO_FLAG ) ) {
                                DBG("Echoing data back\n");
                                if ( sendit_seq( ctx->sock, info.sinfo_ppid, info.sinfo_stream,
                                                        (struct sockaddr *)&peer_ss, peerlen,
                                                        ctx->recvbuf, ctx->recvbuf_size) < 0 ) {
                                        WARN("Error while echoing data!\n");
                                }
                        }
                }
        }

        return 0;
}


/**
 * Signal handler for handling user pressing ctrl+c.
 * @param sig Signal received.
 */
void sighandler( int sig )
{
        DBG("Received signal %d \n", sig );
        if ( sig == SIGPIPE ) {
                WARN("Received SIGPIPE, closing down\n");
        }

        close_req = 1;
}

static void print_usage() 
{
        printf("sctp_server v%s\n", PROG_VERSION );
        printf("Usage: sctp_server [options] \n");
        printf("Available options are: \n" );
        printf("\t--port <port>  : listen on local port <p>, default %d \n", DEFAULT_PORT);
        printf("\t--buf <size>   : Size of rceive buffer is <size>, default is %d\n",
                      RECVBUF_SIZE);
        printf("\t--seq          : use SOCK_SEQPACKET socket instead of SOCK_STREAM\n");
        printf("\t--echo         : Echo the received data back to sender\n");
        printf("\t--verbose      : Be more verbosive \n");
        printf("\t--help         : Print this message \n");
}  

static int parse_args( int argc, char **argv, struct server_ctx *ctx )
{
        int c, option_index;

        struct option long_options[] = {
                { "port", 1, 0, 'p' },
                { "help", 0,0, 'H' },
                { "buf", 1,0,'b' },
                { "seq", 0,0,'s' },
                { "echo",0,0,'e' },
                { "verbose", 0,0,'v'},
                { 0,0,0,0 }
        };

        while (1) {

                c = getopt_long( argc, argv, "p:b:Hsev", long_options, &option_index );
                if ( c == -1 )
                        break;

                switch (c) {
                        case 'p' :
                                if ( parse_uint16( optarg, &(ctx->port)) < 0 ) {
                                        fprintf(stderr, "Malformed port given\n" );
                                        return -1;
                                }
                                break;
                        case 'b' :
                                if ( parse_uint16( optarg, &(ctx->recvbuf_size)) < 0 ) {
                                        fprintf(stderr, "Illegal recv buffer size given\n");
                                        return -1;
                                }
                                break;
                        case 's' :
                                ctx->options = set_flag( ctx->options, SEQPKT_FLAG );
                                break;
                        case 'e' :
                                ctx->options = set_flag( ctx->options, ECHO_FLAG );
                                break;
                        case 'v' :
                                ctx->options = set_flag( ctx->options, VERBOSE_FLAG );
                                break;
                        case 'H' :
                        default :
                                print_usage();
                                return 0;
                                break;
                }
        }

        return 1;
}




int main( int argc, char *argv[] )
{
        struct sockaddr_storage myaddr,remote;
        struct server_ctx ctx;
        struct sctp_event_subscribe event;
        int cli_fd, ret;
        socklen_t addrlen;
        char peer[INET6_ADDRSTRLEN];
        void *ptr;

        if ( signal( SIGTERM, sighandler ) == SIG_ERR ) {
                fprintf(stderr, "Unable to set signal handler\n");
                return EXIT_FAILURE;
        }
        if ( signal( SIGINT, sighandler ) == SIG_ERR ) {
                fprintf(stderr, "Unable to set signal handler\n");
                return EXIT_FAILURE;
        }
        if ( signal( SIGPIPE, sighandler ) == SIG_ERR ) {
                fprintf(stderr, "Unable to set signal handler\n");
                return EXIT_FAILURE;
        }

        memset( &ctx, 0, sizeof( ctx ));
        ctx.port = DEFAULT_PORT;
        ctx.recvbuf_size = RECVBUF_SIZE;

        ret = parse_args( argc, argv, &ctx );
        if ( ret  < 0 ) {
                WARN("Error while parsing command line\n" );
                return EXIT_FAILURE;
        } else if ( ret == 0 ) {
                return EXIT_SUCCESS;
        }
        

        memset( &myaddr, 0, sizeof( myaddr));
        myaddr.ss_family = AF_INET6;

        if ( is_flag( ctx.options, SEQPKT_FLAG )) {
                DBG("Using SEQPKT socket\n");
                ctx.sock = socket( PF_INET6, SOCK_SEQPACKET, IPPROTO_SCTP );
        } else {
                DBG("Using STREAM socket\n");
                ctx.sock = socket( PF_INET6, SOCK_STREAM, IPPROTO_SCTP );
        }
        if ( ctx.sock < 0 ) {
                fprintf(stderr, "Unable to create socket: %s \n", strerror(errno));
                return EXIT_FAILURE;
        }

        if ( bind_and_listen( &ctx ) < 0 ) {
                fprintf(stderr, "Error while initializing the server\n" );
                close(ctx.sock);
                return EXIT_FAILURE;
        }

        if ( is_flag( ctx.options, SEQPKT_FLAG ) 
                        && is_flag( ctx.options, VERBOSE_FLAG) ) {
                memset( &event, 0, sizeof( event ));
                event.sctp_data_io_event = 1;
                if ( setsockopt( ctx.sock, IPPROTO_SCTP, SCTP_EVENTS,
                                        &event, sizeof( event)) != 0 ) {
                        fprintf(stderr, "Unable to subscribe to SCTP IO events: %s \n",
                                        strerror( errno ));
                        unset_flag( ctx.sock, VERBOSE_FLAG);
                }
        }


        memset( &remote, 0, sizeof(remote));
        addrlen = sizeof( struct sockaddr_in6);

        TRACE("Allocating %d bytes for recv buffer \n", ctx.recvbuf_size );
        ctx.recvbuf = mem_alloc( ctx.recvbuf_size * sizeof( uint8_t ));

        while ( !close_req ) {
                if ( is_flag( ctx.options, SEQPKT_FLAG ) ) {
                        if ( do_server_seq( &ctx ) < 0 ) 
                                break;
                } else {
                        cli_fd = do_accept( &ctx, &remote, &addrlen );
                        if ( cli_fd < 0 ) {
                                if ( errno == EINTR ) 
                                        break;

                                close( ctx.sock );
                                mem_free( ctx.recvbuf);
                                WARN( "Error in accept!\n");
                                return EXIT_FAILURE;
                        } else if ( cli_fd == 0 ) {
                                break;
                        }
                        if ( remote.ss_family == AF_INET ) {
                                ptr = &(((struct sockaddr_in *)&remote)->sin_addr);
                        } else {
                                ptr = &(((struct sockaddr_in6 *)&remote)->sin6_addr);
                        }
                        if ( inet_ntop(remote.ss_family, ptr, peer, addrlen ) != NULL ) {
                                printf("Connection from %s \n", peer );
                        } else {
                                printf("Connection from unknown\n");
                        }
                        do_server( &ctx, cli_fd );
                        close( cli_fd );
                }
        }
        mem_free( ctx.recvbuf);
        close( ctx.sock );

        return EXIT_SUCCESS;
}










